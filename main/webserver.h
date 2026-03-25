//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

using status_request_callback_t = void (*)(char *buffer, size_t buffer_size);

void webserver_init(status_request_callback_t status_callback);

// OTA state accessors — for inclusion in /api/status without coupling the
// status handler directly to webserver internals.
const char *webserver_ota_state();    // "idle" | "downloading" | "verifying" | "complete" | "failed"
int         webserver_ota_progress(); // 0-100
