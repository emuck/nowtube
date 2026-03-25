// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#pragma once

// Boot coordinator — initialises all subsystems in the correct order and wires
// up runtime callbacks. Called exactly once from app_main().
void app_boot_run();
