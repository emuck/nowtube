// SPDX-FileCopyrightText: 2025 Martin R. Raumann
// SPDX-License-Identifier: BSD-3-Clause
//
// zvibe_service — Z-machine web terminal service for nowtube (ESP32).
//
// Only compiled when CONFIG_NOWTUBE_ENABLE_ZVIBE_WEBTERM is enabled and
// the zvibe submodule is initialized (ZVIBE_AVAILABLE=1).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ZVIBE_AVAILABLE

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool connected;
    bool game_buffer_allocated;
    const char *game_loaded;
    size_t game_size;
    uint32_t ws_send_fail;
    uint32_t queue_drop;
    uint32_t queue_waiting;
} zvibe_status_snapshot_t;

/**
 * Initialise the zvibe service.
 *
 * Registers HTTP/WebSocket handlers on the existing httpd server (obtained
 * via webserver_get_handle()), allocates the game buffer from PSRAM, creates
 * the FreeRTOS task and message queue.
 *
 * Call once from app_boot_run(), after webserver_init().
 */
void zvibe_service_init(void);

/**
 * Return a point-in-time diagnostics snapshot for /api/status.
 */
zvibe_status_snapshot_t zvibe_service_status(void);

#ifdef __cplusplus
}
#endif

#endif /* ZVIBE_AVAILABLE */
