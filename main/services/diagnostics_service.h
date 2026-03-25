//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include "models/status_snapshot.h"

namespace diagnostics_service {

void init();
diagnostics_snapshot get_snapshot();

}
