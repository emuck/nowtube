//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <ctime>

struct diagnostics_snapshot {
  const char *last_reset_reason{};
  uint32_t boot_count{};
  uint32_t free_heap{};
  uint32_t min_free_heap{};
};

struct wifi_status_snapshot {
  bool connected{};
  const char *ip{};
  uint32_t retry_count{};
};

struct build_info_snapshot {
  const char *firmware_version{};
  const char *git_sha{};
  const char *build_time_utc{};
  const char *asset_rev{};
  bool git_dirty{};
};

struct status_snapshot {
  build_info_snapshot build{};
  uint32_t uptime_s{};
  const char *mode{};
  uint8_t brightness_pct{};
  wifi_status_snapshot wifi{};
  bool weather_available{};
  time_t last_weather_sync{};
  diagnostics_snapshot diagnostics{};
};
