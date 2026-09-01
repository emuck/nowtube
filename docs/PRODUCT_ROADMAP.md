# nowtube Product Roadmap

## Product position

nowtube is a calm, self-contained living-room clock for the original Nextube
hardware. It should feel designed, legible, and pleasantly constrained—not
like a small general-purpose dashboard.

The product advantage is not feature count. It is the combination of:

- a clean six-panel ambient briefing;
- bespoke, coherent artwork rather than generic status glyphs;
- excellent typography at across-the-room viewing distance;
- local configuration and dependable updates; and
- small, delightful interactions, including games that fit the physical
  controls and six-panel layout.

## The comparison in one sentence

Nextube-Remaster is a useful hardware and feature reference; nowtube should
borrow its strongest product mechanics while preserving a focused visual
language and a much smaller decision surface.

## Product rules

1. **One clear default.** A new feature must have an opinionated default and
   work without a setup page full of choices.
2. **Art-directed, not merely decorated.** Icons, type, colours, and motion
   belong to a named visual system and are reviewed together.
3. **Readable at a glance.** A panel communicates one fact. Animation is used
   to reveal or soften, never to compete with the time.
4. **Local and durable.** No account, cloud relay, or companion server is
   required for the core experience.
5. **No feature without a removal story.** Every mode consumes attention,
   flash, RAM, UI space, and test surface. Features must be removable or
   deliberately maintained.

## Current foundation

Already shipped on `main`:

- Clock, TODAY, and FORECAST ambient modes with local weather, RTC/NTP, and
  OTA firmware updates.
- Nixie and Space Mono clock faces, plus a developer Google Fonts conversion
  tool.
- Replaceable SPIFFS artwork and asset revision handling.
- Tube Invaders, which uses the six portrait panels and three touch controls
  as a native game surface rather than an afterthought.

Parked on `feature/microphone-harmonic-landscape`:

- GPIO35 / ADC1_CH7 microphone capture, Spectrum/Harmonic Landscape rendering,
  diagnostics, SHT30 exploration, and related memory experiments.
- This is intentionally not part of the product roadmap until the physical
  microphone can be validated and a visual use earns its complexity.

## Prioritized roadmap

### R1 — Typography as a first-class product feature

**Goal:** Turn the existing conversion script into a curated, low-risk font
library that users can select on-device.

1. Define font roles: clock digits, compact panel labels, and large panel
   values. Do not use one typeface indiscriminately everywhere.
2. Ship a small initial pack (roughly four to six faces), each with a visual
   purpose, preview image, licence record, glyph coverage, and memory budget.
   Candidate directions: refined nixie, technical mono, soft rounded, and
   high-legibility geometric.
3. Extend `font_convert.py` to generate a font manifest and the CMake entries
   from a Google Font or local OFL/Apache/MIT-licensed TTF.
4. Add a web UI font picker with an immediate preview and a safe persisted
   selection. The first version selects from **compiled, curated packs**.
5. Add a contributor workflow for proposing a font: licence check, required
   glyph set, generated-size report, screenshot, and physical-device review.

**Deliberate non-goal:** arbitrary on-device TTF upload/rendering. It is a
large renderer/cache/filesystem system for an experience that can be excellent
with carefully compiled assets. Reconsider only after the curated path proves
too limiting.

### R2 — Bespoke visual system and icon packs

**Goal:** Make the six panels feel like one designed object.

1. Establish a compact art-direction guide: stroke/edge treatment, palette,
   contrast, spacing, icon scale, and dark-mode background rules.
2. Create a semantic icon manifest (`sun`, `rain`, `wind`, `humidity`, `aqi`,
   `sunrise`, and so on) rather than spreading raw filenames through display
   code.
3. Ship one polished default icon pack, then one deliberately different
   alternate pack—not an open-ended theme marketplace.
4. Package a font + icon pack + palette as a versioned **Nowtube Look**.
   Selecting a Look changes the coherent set; advanced per-piece overrides can
   wait.
5. Add image-generation/source and export guidance so each new asset has a
   reproducible source, fixed 80×160/asset dimensions, alpha rules, and a
   device screenshot review.

### R2.5 — Browser Look Studio

**Goal:** Make visual choices reviewable before a firmware build or device
flash.

**Current checkpoint:** a local CLOCK preview is available at
`tools/look-studio/index.html`, covering the first curated font pack,
brightness, time, and the approved sunny icon. TODAY, FORECAST, reusable Look
bundles, and day/night fixtures remain later work.

1. Build a local browser preview that renders six 80×162 virtual tubes with
   the real physical gaps and a scaled desktop view.
2. Render the same semantic content as CLOCK, TODAY, and FORECAST using a
   shared preview data fixture: time, weather state, temperatures, wind,
   humidity/AQI, and sun event.
3. Let a reviewer switch among curated fonts, Looks, icon packs, brightness,
   and representative day/night states without changing device configuration.
4. Use the original approved TTF/WOFF files for quick browser previews and the
   same source PNG assets intended for SPIFFS. The final approval remains a
   screenshot from the compiled LVGL/device build; browser and device raster
   output will not be assumed pixel-identical.
5. Keep it static and local at first—no ESP32 emulator, Wi-Fi simulator, or
   duplicated firmware business logic. Its job is art direction, not hardware
   validation.

**Why it comes early:** it gives font and icon choices an immediate review
loop, and it becomes the contact-sheet/visual-regression surface for every
future Look.

### R3 — Simple release bundles and asset safety

**Goal:** Borrow the remaster's good update ergonomics without its online
updater, file browser, or release-management complexity.

1. Define a single nowtube release bundle containing the app image, SPIFFS
   assets, version manifest, and checksums.
2. In the web UI, detect an app/asset revision mismatch and give one concise
   repair instruction or upload action.
3. Preserve the current explicit OTA and SPIFFS paths as the recovery route.
4. Provide a one-click local export/import of user configuration before any
   full asset refresh.

### R4 — Ambient refinement, not mode proliferation

**Goal:** Increase everyday delight in the three existing ambient modes.

Prioritize, in order:

1. a moon-phase or night-sky treatment that respects the existing calm layout;
2. a stronger weather-state narrative through bespoke condition art and subtle
   phase transitions;
3. optional indoor comfort data only if the on-board sensor is consistently
   validated; and
4. a small number of user-chosen panel substitutions, with a recommended
   default layout always visible.

Avoid adding separate date, alarm, timer, slideshow, social-counter, and
arbitrary information modes merely because the hardware can display them.

### R5 — Games as a nowtube signature

**Goal:** Make games a small, intentional collection rather than a hidden
novelty.

1. Add a game launcher with Tube Invaders as the default and a clear exit path.
2. Define a tiny game SDK: six-lane/panel coordinate model, touch input,
   sound effects, LED cues, frame budget, pause/exit behaviour, and NVS score
   namespace.
3. Add one second game only after the launcher is solid. Good candidates are a
   six-panel rhythm/pattern game or a calm score-chasing puzzle; both use the
   physical arrangement as part of the game design.
4. Keep games offline, instantly available, and separate from ambient-mode
   configuration.

### R6 — Quiet reliability improvements

**Goal:** Make the refined product feel boringly dependable.

- Finish RTC validation/discipline only if it produces measurable benefit.
- Keep Wi-Fi recovery and local setup simple and testable.
- Track heap, asset revision, and update status without exposing a debugging
  console as a consumer feature.
- Add hardware screenshot/soak checks for every Look and game before release.

## Explicitly deferred or rejected

| Area | Decision | Why |
| --- | --- | --- |
| Microphone / spectrum | Parked on feature branch | Hardware response and visual payoff are not yet compelling. |
| Social counters and relays | Reject | External credentials/services conflict with the self-contained goal. |
| MQTT / Home Assistant / WLED | Reject for core | Useful to enthusiasts, but widens setup and support burden. |
| Album/slideshow and file manager | Reject | Consumes UI/storage complexity without serving the clock's identity. |
| Large theme rotation matrix | Reject | Reduces visual coherence and creates choice overload. |
| Arbitrary runtime TTF renderer | Defer | Curated compiled packs are smaller, faster, and easier to validate. |
| One-click internet updater | Defer | A local release bundle gives most of the benefit with less trust and failure surface. |

## Success measures

- A new owner can configure the clock and choose a Look in under five minutes.
- A viewer can identify the time and current weather state from across a room.
- Every shipped Look has coherent clock, panel, and icon treatment—not merely a
  different digit font.
- Asset updates are recoverable and do not require understanding partitions.
- A game feels purpose-built for six portrait panels and three buttons.
- The default web UI remains short enough to scan in one sitting.

## Suggested execution order

1. R1 font-pack/manifest foundation.
2. R2.5 Browser Look Studio, initially for the curated font pack.
3. R2 default Look and semantic icon manifest.
4. R3 release-bundle/update safety.
5. R4 first ambient refinement.
6. R5 game launcher, then one additional game.
7. R6 reliability work continuously, with microphone remaining parked.
