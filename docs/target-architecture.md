# Target Architecture

## Goals

This document proposes the next target architecture for nowtube as it moves from a prototype toward production-grade embedded firmware.

The priorities are:

- Simplicity over cleverness
- Clear ownership of state and hardware
- Predictable concurrency
- Recoverable failures instead of hard resets
- Strong observability and field diagnostics
- Testability with a TDD-friendly structure
- Clean separation between product logic and platform plumbing
- An architecture suitable for open source publication

This is not a rewrite-from-scratch plan. It is a target shape that should guide incremental refactoring.

## Product Direction

The device should become self-contained:

- The ESP32 serves a small local configuration UI
- The ESP32 owns weather, time, display mode, and local settings
- External backend-driven display control is removed
- The device remains useful even if no companion system exists

The new web server is a local management surface, not a remote orchestration interface.

Current implementation status:

- local static UI is now present at `GET /`
- `GET /api/status`, `GET /api/config`, and `POST /api/config` are live
- config is persisted in NVS through `config_service`
- Wi-Fi, timezone, weather settings, and brightness now flow through the typed config model
- the web UI is still intentionally minimal and should be treated as the first shipping scaffold, not the finished product

The display direction should also be simplified:

- Remove split-flap animation from the default product path
- Remove the split-flap background graphic from the default UI
- Prefer a clean, fast, typography-driven display using bundled fonts
- Treat animation as a possible future enhancement, not a current requirement

## Design Principles

### 1. Single Owner Per Domain

Each subsystem should have one clear owner:

- UI state owned by the display task
- Network/config state owned by service/controller tasks
- Hardware drivers owned by dedicated modules

Cross-task access to shared mutable state should be replaced by message passing where practical.

### 2. Bounded Concurrency

The current system mixes FreeRTOS tasks, esp timers, LVGL async work, event loop callbacks, and direct cross-context calls. The target system should reduce this.

Preferred model:

- A small number of long-lived tasks
- Queues for communication
- Minimal ad hoc task spawning
- Minimal use of global mutable state

### 3. Fail Soft

Transient failures should not reboot the device.

Preferred behavior:

- Log the error
- Record a metric or fault counter
- Degrade gracefully
- Retry with backoff when appropriate

Use `ESP_ERROR_CHECK` only for true boot-time invariants where continuing would be invalid.

### 4. Thin Drivers, Testable Services

Drivers should be small and hardware-focused.

Business logic should live in service/controller code that can be tested on host without real hardware.

### 5. Configuration Is a First-Class Service

Configuration should not be spread across raw globals, text files, and ad hoc parsing. The firmware should have one configuration model and one persistence layer.

## Proposed Top-Level Architecture

### Boot Coordinator

Responsible for:

- NVS init
- SPIFFS init
- crash/reboot diagnostics init
- config load
- driver init
- service start order
- fallback behavior if one subsystem fails

This should be a small orchestrator, not the place where product logic lives.

Suggested file:

- `main/app_boot.cpp`

### Config Service

Responsible for:

- Loading persisted configuration from NVS
- Validating configuration
- Applying defaults
- Saving updates atomically
- Exposing a typed config model to the rest of the firmware

Suggested config model:

- Wi-Fi credentials
- timezone
- weather provider settings
- units
- brightness
- feature flags
- hostname / device name

Suggested files:

- `main/services/config_service.h`
- `main/services/config_service.cpp`
- `main/models/device_config.h`

Important constraints:

- No raw `char[]` config globals shared across modules
- One schema version
- Migrations handled in one place

### Status Service

Responsible for:

- Current uptime
- last weather sync time
- Wi-Fi connection state
- IP address
- current mode
- brightness
- reboot reason
- fault counters
- heap watermarks
- task stack high-water marks

This becomes the source for `/api/status` and diagnostic logs.

Suggested files:

- `main/services/status_service.h`
- `main/services/status_service.cpp`

### Weather Service

Responsible for:

- Scheduling weather refreshes
- Performing HTTP fetches
- Parsing provider responses
- Backoff/retry policy
- Publishing a weather update event/message

It should not directly manipulate LVGL widgets.

Suggested files:

- `main/services/weather_service.h`
- `main/services/weather_service.cpp`
- `main/models/weather_data.h`

Target behavior:

- One long-lived service task or one service state machine
- No direct UI updates from fetch task
- Clear timeout behavior
- Retry with bounded backoff
- Persist last successful weather snapshot if useful

### Display Controller

Responsible for:

- Owning display state
- Handling mode transitions
- Receiving messages like:
  - time changed
  - weather updated
  - user changed brightness
  - config changed
- Updating LVGL only from the display/UI task context

Suggested files:

- `main/controllers/display_controller.h`
- `main/controllers/display_controller.cpp`

This controller should be the only module that knows how product state maps to LVGL widgets.

The first target display implementation should be static and lightweight:

- fast number and text updates
- no snapshot-based flapper animation in the shipping path
- no dependency on split-flap themed assets for normal operation
- clear typography using bundled fonts

### Input Controller

Responsible for:

- Translating touch/button events into domain actions
- Short press / long press behavior
- Dispatching messages rather than directly mutating many modules

Suggested files:

- `main/controllers/input_controller.h`
- `main/controllers/input_controller.cpp`

### Backlight Service

Responsible for:

- Backlight brightness
- LED color state
- breathing/mixed effects
- effect timers

Near-term note:

- the remaining occasional blink on the tail LED during breathing mode should be tracked as a driver-quality enhancement, not as justification to reintroduce multi-owner LED control paths
- reversing logical LED order during diagnostics did not move the visible artifact away from the same physical right-most display

It should have its own state and API instead of mixing policy into `main.cpp`.

Suggested files:

- `main/services/backlight_service.h`
- `main/services/backlight_service.cpp`

### UI Server

Responsible for:

- Serving static assets from SPIFFS
- Serving a small config/status API
- Authentication strategy if needed later
- Translating HTTP requests into config updates or service actions

Suggested endpoints:

- `GET /`
- `GET /app.js`
- `GET /app.css`
- `GET /api/status`
- `GET /api/config`
- `POST /api/config`
- `POST /api/actions/reboot`
- `POST /api/actions/weather-refresh`
- `POST /api/actions/firmware`

Endpoints removed:

- `/api/display` ✅
- `/webhook` ✅

### OTA Update Model

The preferred OTA architecture should support two local-network update flows with one underlying firmware path:

1. Browser-upload OTA
   The user opens the local config UI, picks a firmware file from their computer, and the browser uploads it to the device.

2. Host-pushed OTA
   A trusted host or local admin tool uploads the same firmware file directly to the device over HTTP.

These two flows should converge on the same OTA implementation in firmware:

- receive image bytes
- validate image metadata
- write to the inactive OTA partition
- set the new boot partition
- reboot
- rely on ESP-IDF rollback-safe OTA behavior

URL-based self-update can be added later if needed, but it should not come before the upload-based flows.

*Implementation note: in practice, URL-based OTA (`POST /api/ota {"url":"..."}`) was built first because it was needed for development iteration. Browser-upload OTA remains the next planned step. The rollback, SHA-256 verification, and progress-reporting requirements are all met by the current implementation.*

- backend-driven display orchestration endpoints

Suggested files:

- `main/http/ui_server.h`
- `main/http/ui_server.cpp`
- `main/http/json_helpers.h`
- `main/http/json_helpers.cpp`

## Runtime Task Model

Target runtime model:

- `display_task`
  - Owns LVGL
  - Processes UI messages from a queue
- `service_task`
  - Owns periodic service scheduling or receives timer events
  - Runs weather refresh state machine
- `system_event_task`
  - ESP event loop callbacks feed into queues, not directly into UI logic

Optional:

- Keep HTTP server in its own ESP-IDF-managed context
- Avoid custom short-lived tasks for one-off operations unless required

## Message Passing Model

Introduce explicit events/messages between modules.

Examples:

```cpp
enum class AppEventType {
  WeatherUpdated,
  TimeUpdated,
  WifiConnected,
  WifiDisconnected,
  ConfigChanged,
  BrightnessChanged,
  ButtonPressed,
  ButtonLongPressed,
};
```

Messages should be small, typed, and owned by queues or ring buffers.

Benefits:

- Easier reasoning about state changes
- Better logging
- Better unit tests
- Clear task boundaries

## Logging and Diagnostics

The codebase should adopt enterprise-grade embedded logging, meaning:

- Structured and consistent log messages
- Stable subsystem tags
- Clear lifecycle logs
- Error codes included in failure logs
- Throttling for repeated failures
- Persisted crash and reboot reason

### Logging Standard

Each module should log:

- startup
- configuration summary
- important state transitions
- warnings on degraded operation
- errors with enough context to diagnose remotely

Example:

```text
I config: loaded config version=3 units=imperial timezone=PST8PDT
I weather: refresh started provider=openweathermap attempt=1
W weather: refresh failed err=ESP_ERR_HTTP_CONNECT status=0 retry_in_s=60
I display: mode changed from=CLOCK to=WEATHER source=weather_update
```

### Diagnostic Persistence

Persist in NVS:

- last reboot reason
- reset counter
- watchdog counter
- last successful weather sync timestamp
- last critical fault summary

### Health Endpoint

`GET /api/status` should return:

- firmware version
- uptime
- free heap
- minimum free heap
- Wi-Fi state
- IP
- current mode
- current brightness
- last weather sync
- reboot reason
- fault counters

## Configuration UI

The UI should remain intentionally small.

Suggested sections:

- Device status
- Wi-Fi settings
- Timezone
- Weather settings
- Units
- Display mode settings
- Brightness / backlight settings
- Diagnostics / reboot

Constraints:

- No heavy frontend framework required
- Plain HTML/CSS/JS is sufficient
- Keep assets small for SPIFFS
- Prioritize reliability over polish

If complexity grows later, the UI can still remain framework-free by using small ES modules.

## Error Handling Policy

### Boot-Time Fatal

Only fatal if the device cannot safely operate at all.

Examples:

- NVS cannot initialize after recovery attempt
- essential display hardware init fails and product is unusable

### Runtime Recoverable

Should log and continue:

- weather fetch failure
- Wi-Fi disconnect
- config validation failure for one field
- HTTP bad request
- transient queue full

### Assertions

Use assertions for developer-only invariants, not for user-triggerable runtime conditions.

Production builds should minimize assert-driven resets in hot paths.

## Testing Strategy

The target architecture should support TDD.

### Test Pyramid

#### Unit Tests

Host-run tests for:

- config validation and migration
- weather parsing
- retry/backoff policy
- mode transition rules
- button action mapping
- status serialization

These should be the bulk of the test suite.

#### Integration Tests

Host or simulator tests for:

- HTTP config API behavior
- persistence round-trips
- service-controller interactions

#### Hardware Tests

Limited on-device tests for:

- display init
- touch events
- Wi-Fi connectivity
- SPIFFS/NVS integration

### Design for Testability

To enable this:

- Put business logic behind interfaces
- Separate hardware adapters from pure logic
- Avoid global singletons in new code
- Inject time, storage, HTTP, and transport dependencies where practical

Example seams:

- `IClock`
- `IStorage`
- `IWeatherClient`
- `IWifiManager`

### CI Goals

At minimum:

- host unit tests on every PR ✅ (`host-tests` job in `.github/workflows/build.yml`)
- firmware build on every PR ✅ (`firmware` job in `.github/workflows/build.yml`)
- formatting/lint checks — not yet implemented

Later:

- simulator-based smoke test
- artifact generation for review builds

## Coding Standards

Recommended standards for production readiness:

- No new product logic in `main.cpp`
- No direct cross-module global state mutation
- No direct LVGL calls outside display-owned code
- No raw stringly-typed modes across modules
- Prefer typed structs/enums over ad hoc JSON-driven state
- One module, one responsibility
- Explicit return-value checking

## Refactor Phases

### Phase 1: Stabilize

- Stop adding features in current architecture
- Fix watchdog and thread-affinity issues
- Remove dead backend-control paths
- Add reboot/fault diagnostics

### Phase 2: Extract Services

- Extract config service
- Extract display controller
- Extract weather service
- Extract backlight service

### Phase 3: Replace Remote-Control Webserver ✅

- `/api/display` and webhook control flow removed
- Status/config endpoints live
- Local config UI served from SPIFFS

### Phase 4: Test Foundation

- Add host unit-test target
- Add parser/config/mode tests
- Add CI build and test pipeline

### Phase 5: Driver Hardening

- Rework LCD driver lifecycle
- Reduce blocking behavior in hot paths
- Clarify ownership of timers and queues

## Open Source Readiness

To be proud of open sourcing, the repo should reach this standard:

- clear architecture docs
- clear module boundaries
- stable build instructions
- example configuration
- test suite that passes in CI
- minimal surprising globals
- well-named files and APIs
- logging and diagnostics that help contributors debug issues

Suggested supporting docs:

- `docs/architecture-overview.md`
- `docs/runtime-model.md`
- `docs/testing-strategy.md`
- `docs/logging-standard.md`
- `docs/config-schema.md`

## Confirmed Architectural Decisions

These decisions are accepted and guide all refactoring work:

1. The ESP32 is the sole product controller; no external backend drives display state.
2. The web server exists only for local config/status/actions.
3. LVGL is owned by exactly one task.
4. Weather/network services publish events; they do not touch UI directly.
5. The default display path is static and typography-driven; flapper animation is deferred.
6. New product logic must be placed in extracted services/controllers, not `main.cpp`.
