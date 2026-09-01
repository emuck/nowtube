//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

using wifi_connection_callback_t = void (*)(bool connected);
using wifi_recovery_callback_t = void (*)();

void wifi_init(wifi_connection_callback_t connection_callback,
               wifi_recovery_callback_t recovery_callback);
bool wifi_connect(const char *ssid, const char *psk);

// Start an open recovery softAP named "nowtube-setup" (192.168.4.1).
// Stops STA first if it was previously started.  Idempotent: calling a
// second time is safe (AP netif is created only once).  A failed station
// connection also enters this mode automatically after a short timeout.
void wifi_start_recovery_ap();

// Stop the recovery AP and clear the recovery-mode guard so STA reconnects
// normally on the next wifi_connect() call.
void wifi_cancel_recovery_ap();
