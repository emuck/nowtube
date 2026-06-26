// SPDX-FileCopyrightText: 2025 Martin R. Raumann
// SPDX-License-Identifier: BSD-3-Clause
//
// zvibe_service.cpp — ESP32 platform adapter for the zvibe webterm target.
//
// Architecture
// ------------
//   HTTP server task  →  msg queue  →  zvibe task
//
// The esp_http_server WebSocket callback runs on the HTTP server task.
// It pushes lightweight messages onto a FreeRTOS queue; the zvibe task
// dequeues them, runs the Z-machine, and sends output back via
// httpd_ws_send_frame_async() (which is safe to call from any task).
//
// Games are stored as *.z3 files anywhere under /spiffs/ and uploaded via
// the existing /api/spiffs/upload endpoint.
// Save-game state is persisted to NVS under the "zvibe" namespace.

#include "zvibe_service.h"

#ifdef ZVIBE_AVAILABLE

#include <cstdio>
#include <cstring>
#include <dirent.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "webterm_platform.h"
#include "webterm_session.h"
#include "webserver.h"

static const char *TAG = "zvibe";

// ---------------------------------------------------------------------------
// Embedded HTML asset

extern const char zvibe_html_start[] asm("_binary_zvibe_html_start");
extern const char zvibe_html_end[]   asm("_binary_zvibe_html_end");

// ---------------------------------------------------------------------------
// Platform adapter

typedef struct {
    webterm_platform_t base;   /* must be first */
    httpd_handle_t     server;
    int                fd;          /* active WebSocket fd; -1 if none */
    char               game[64];   /* currently loaded game filename (no path) */
} esp32_platform_t;

static esp32_platform_t s_platform;

// ---------------------------------------------------------------------------
// Message queue (HTTP task → zvibe task)

typedef enum {
    ZVIBE_MSG_CONNECT,
    ZVIBE_MSG_FRAME,
    ZVIBE_MSG_DISCONNECT,
} zvibe_msg_type_t;

#define ZVIBE_FRAME_MAX 512

typedef struct {
    zvibe_msg_type_t type;
    int              fd;
    char             payload[ZVIBE_FRAME_MAX];
    size_t           len;
} zvibe_msg_t;

static QueueHandle_t s_msg_queue = nullptr;
static TaskHandle_t  s_task      = nullptr;
static uint32_t      s_ws_send_fail = 0;
static uint32_t      s_queue_drop   = 0;

// ---------------------------------------------------------------------------
// Session

static webterm_session_t *s_session = nullptr;

// Game data buffer (128 KB, allocated from PSRAM at init).
// Must outlive the session — kept alive for the lifetime of the service.
static uint8_t *s_game_buf  = nullptr;
static size_t   s_game_size = 0;

// ---------------------------------------------------------------------------
// Platform callbacks

static void esp32_send(webterm_platform_t *p, const char *json, size_t len)
{
    esp32_platform_t *pp = (esp32_platform_t *)p;
    if (pp->fd < 0 || !pp->server) return;

    httpd_ws_frame_t frame = {};
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)json;
    frame.len     = len;
    frame.final   = true;

    esp_err_t err = httpd_ws_send_frame_async(pp->server, pp->fd, &frame);
    if (err != ESP_OK) {
        s_ws_send_fail++;
        ESP_LOGW(TAG, "ws send failed fd=%d: %s", pp->fd, esp_err_to_name(err));
        // Signal the zvibe task to disconnect
        zvibe_msg_t msg = {};
        msg.type = ZVIBE_MSG_DISCONNECT;
        msg.fd   = pp->fd;
        if (xQueueSend(s_msg_queue, &msg, 0) != pdTRUE) {
            s_queue_drop++;
        }
    }
}

static void esp32_close_ws(httpd_handle_t server, int fd)
{
    if (!server || fd < 0) return;

    httpd_ws_frame_t frame = {};
    frame.type  = HTTPD_WS_TYPE_CLOSE;
    frame.final = true;
    esp_err_t err = httpd_ws_send_frame_async(server, fd, &frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws close failed fd=%d: %s", fd, esp_err_to_name(err));
    }
}

/* Build the save file path for the current game into @p out (size @p max).
 * Uses the game filename stem (strips .z3 suffix) as the key, e.g.:
 *   "zork1-r88-s840726.z3" → "/spiffs/zvibe_zork1-r88-s840726.sav"
 * Falls back to "/spiffs/zvibe.sav" if no game is loaded. */
static void save_path(const esp32_platform_t *pp, char *out, size_t max)
{
    if (pp->game[0] == '\0') {
        snprintf(out, max, "/spiffs/game.sav");
        return;
    }
    /* Strip .z3 extension, take first 10 chars, strip trailing '-'. */
    char stem[65];
    snprintf(stem, sizeof(stem), "%s", pp->game);
    size_t n = strlen(stem);
    if (n > 3 && strcmp(stem + n - 3, ".z3") == 0)
        stem[n - 3] = '\0';
    stem[10] = '\0';
    n = strlen(stem);
    while (n > 0 && stem[n - 1] == '-') stem[--n] = '\0';
    snprintf(out, max, "/spiffs/%s.sav", stem);
}

static int esp32_save(webterm_platform_t *p, const void *data, size_t len)
{
    char path[96];
    save_path((esp32_platform_t *)p, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "save: fopen failed: %s", path); return 0; }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) { ESP_LOGW(TAG, "save: short write %zu/%zu", written, len); return 0; }
    ESP_LOGI(TAG, "saved %zu bytes to %s", len, path);
    return 1;
}

static size_t esp32_restore(webterm_platform_t *p, void *buf, size_t max)
{
    char path[96];
    save_path((esp32_platform_t *)p, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "restore: no save at %s", path); return 0; }
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    ESP_LOGI(TAG, "restored %zu bytes from %s", n, path);
    return n;
}

// ---------------------------------------------------------------------------
// Helpers

/* Simple JSON string field extractor (same logic as posix/main.c). */
static int json_str(const char *json, const char *key,
                    char *out, size_t max)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < max) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

/* Scan /spiffs/ for .z3 files and send {"type":"games","list":[...]}. */
static void send_games_list(void)
{
    char buf[4096];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "{\"type\":\"games\",\"list\":[");
    int first = 1;

    DIR *d = opendir("/spiffs");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            size_t nlen = strlen(e->d_name);
            if (nlen > 3 && strcmp(e->d_name + nlen - 3, ".z3") == 0) {
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                                        "%s\"%s\"", first ? "" : ",", e->d_name);
                first = 0;
            }
        }
        closedir(d);
    } else {
        ESP_LOGW(TAG, "opendir /spiffs failed");
    }

    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    esp32_send(&s_platform.base, buf, strlen(buf));
}

/* Load a .z3 file from /spiffs/ into s_game_buf. */
static bool load_game(const char *filename)
{
    if (!s_game_buf) return false;
    // Reject path traversal
    if (strchr(filename, '/') || strchr(filename, '\\')) return false;

    char path[64];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || (size_t)len > 128 * 1024) { fclose(f); return false; }

    size_t n = fread(s_game_buf, 1, (size_t)len, f);
    fclose(f);
    if (n != (size_t)len) return false;

    s_game_size = n;
    return true;
}

/* Run the session until it needs input or stops. */
static void run_session(void)
{
    webterm_result_t r;
    do { r = webterm_session_run(s_session); } while (r == WEBTERM_OK);
}

/* Dispatch a received WebSocket text frame to the session. */
static void dispatch_frame(const char *json, size_t /*len*/)
{
    char type[32];
    if (!json_str(json, "type", type, sizeof(type))) return;

    if (strcmp(type, "input") == 0) {
        char text[WEBTERM_INPUT_MAX + 1];
        if (json_str(json, "text", text, sizeof(text)))
            webterm_session_on_input(s_session, text, strlen(text));

    } else if (strcmp(type, "load") == 0) {
        char game[64];
        if (json_str(json, "game", game, sizeof(game))) {
            if (load_game(game)) {
                snprintf(s_platform.game, sizeof(s_platform.game), "%s", game);
                webterm_session_load(s_session, s_game_buf, s_game_size);
            } else {
                const char *err =
                    "{\"type\":\"error\",\"message\":\"Game not found\"}";
                esp32_send(&s_platform.base, err, strlen(err));
            }
        }

    } else if (strcmp(type, "reset") == 0) {
        webterm_session_on_reset(s_session);

    } else if (strcmp(type, "save") == 0) {
        webterm_session_on_save(s_session);

    } else if (strcmp(type, "restore") == 0) {
        webterm_session_on_restore(s_session);

    } else if (strcmp(type, "games") == 0) {
        send_games_list();
    }
}

// ---------------------------------------------------------------------------
// zvibe task

static void zvibe_task(void * /*arg*/)
{
    zvibe_msg_t msg;
    while (true) {
        if (xQueueReceive(s_msg_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        switch (msg.type) {

        case ZVIBE_MSG_CONNECT:
            ESP_LOGI(TAG, "client connected fd=%d", msg.fd);
            if (s_platform.fd >= 0 && s_platform.fd != msg.fd) {
                ESP_LOGW(TAG, "client takeover new_fd=%d previous_fd=%d",
                         msg.fd, s_platform.fd);
                esp32_close_ws(s_platform.server, s_platform.fd);
                webterm_session_disconnect(s_session);
            }
            s_platform.fd = msg.fd;
            if (!webterm_session_accept(s_session)) {
                ESP_LOGW(TAG, "client rejected as busy fd=%d", msg.fd);
                esp32_close_ws(s_platform.server, msg.fd);
                s_platform.fd = -1;
                break;
            }
            run_session();
            break;

        case ZVIBE_MSG_FRAME:
            if (msg.fd == s_platform.fd) {
                dispatch_frame(msg.payload, msg.len);
                run_session();
            }
            break;

        case ZVIBE_MSG_DISCONNECT:
            if (msg.fd == s_platform.fd) {
                ESP_LOGI(TAG, "client disconnected fd=%d", msg.fd);
                webterm_session_disconnect(s_session);
                s_platform.fd = -1;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP / WebSocket handlers

static esp_err_t zvibe_html_handler(httpd_req_t *req)
{
    size_t len = (size_t)(zvibe_html_end - zvibe_html_start);
    if (len > 0 && zvibe_html_start[len - 1] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, zvibe_html_start, (ssize_t)len);
    return ESP_OK;
}

static esp_err_t zvibe_ws_handler(httpd_req_t *req)
{
    zvibe_msg_t msg = {};

    if (req->method == HTTP_GET) {
        msg.type = ZVIBE_MSG_CONNECT;
        msg.fd   = httpd_req_to_sockfd(req);
        if (xQueueSend(s_msg_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_queue_drop++;
        }
        return ESP_OK;
    }

    // Get frame length
    httpd_ws_frame_t ws_pkt = {};
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) return err;

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        msg.type = ZVIBE_MSG_DISCONNECT;
        msg.fd   = httpd_req_to_sockfd(req);
        if (xQueueSend(s_msg_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_queue_drop++;
        }
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.len > 0) {
        size_t cap = ws_pkt.len < ZVIBE_FRAME_MAX - 1
                     ? ws_pkt.len : ZVIBE_FRAME_MAX - 1;
        uint8_t buf[ZVIBE_FRAME_MAX] = {};
        ws_pkt.payload = buf;
        err = httpd_ws_recv_frame(req, &ws_pkt, cap);
        if (err != ESP_OK) return err;

        msg.type = ZVIBE_MSG_FRAME;
        msg.fd   = httpd_req_to_sockfd(req);
        msg.len  = ws_pkt.len;
        memcpy(msg.payload, buf, ws_pkt.len);
        if (xQueueSend(s_msg_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_queue_drop++;
        }
    }

    return ESP_OK;
}

static const httpd_uri_t s_zvibe_html_uri = {
    .uri       = "/zvibe.html",
    .method    = HTTP_GET,
    .handler   = zvibe_html_handler,
    .user_ctx  = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

static const httpd_uri_t s_zvibe_ws_uri = {
    .uri          = "/zvibe",
    .method       = HTTP_GET,
    .handler      = zvibe_ws_handler,
    .user_ctx     = nullptr,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = nullptr,
};

// ---------------------------------------------------------------------------
// Public API

void zvibe_service_init(void)
{
    // Allocate game buffer from PSRAM
    s_game_buf = (uint8_t *)heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    if (!s_game_buf) {
        ESP_LOGE(TAG, "failed to allocate game buffer from PSRAM");
        return;
    }

    // Create session
    s_platform.base   = { esp32_send, esp32_save, esp32_restore };
    s_platform.server = static_cast<httpd_handle_t>(webserver_get_handle());
    s_platform.fd     = -1;
    s_platform.game[0] = '\0';

    s_session = webterm_session_create(&s_platform.base);
    if (!s_session) {
        ESP_LOGE(TAG, "failed to create webterm session");
        return;
    }
    webterm_session_set_random_seed(s_session, (int)esp_random());

    // Register URI handlers on the shared HTTP server
    httpd_handle_t server = s_platform.server;
    httpd_register_uri_handler(server, &s_zvibe_html_uri);
    httpd_register_uri_handler(server, &s_zvibe_ws_uri);

    // Message queue and task
    s_msg_queue = xQueueCreate(8, sizeof(zvibe_msg_t));
    xTaskCreate(zvibe_task, "zvibe", 32768, nullptr, 3, &s_task);

    ESP_LOGI(TAG, "zvibe service started — open http://<ip>/zvibe.html");
}

zvibe_status_snapshot_t zvibe_service_status(void)
{
    zvibe_status_snapshot_t snap = {};
    snap.enabled = true;
    snap.connected = s_platform.fd >= 0;
    snap.game_buffer_allocated = s_game_buf != nullptr;
    snap.game_loaded = s_platform.game[0] != '\0' ? s_platform.game : "";
    snap.game_size = s_game_size;
    snap.ws_send_fail = s_ws_send_fail;
    snap.queue_drop = s_queue_drop;
    snap.queue_waiting = s_msg_queue != nullptr
        ? static_cast<uint32_t>(uxQueueMessagesWaiting(s_msg_queue))
        : 0;
    return snap;
}

#endif /* ZVIBE_AVAILABLE */
