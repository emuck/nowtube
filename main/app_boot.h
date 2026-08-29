// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#pragma once

// Boot coordinator — initialises all subsystems in the correct order and wires
// up runtime callbacks. Called exactly once from app_main().
void app_boot_run();

// Reschedules the auto-cycle timer after an explicit/manual mode change.
void app_boot_reschedule_current_mode_cycle();
