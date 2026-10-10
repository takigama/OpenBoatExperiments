# MD2030 Marine Engine Panel — HELM display + CYD secondary displays

Custom engine panel for a Volvo Penta MD2030 diesel (sailboat). This one
sketch now builds for **4 hardware targets**, picked at compile time via
`board_select.h`'s `TARGET_BOARD` macro:

- **HELM** (`BOARD_HELM_S3_800x480`): the primary node, a VIEWE
  UEDX80480070E-WB-A board (ESP32-S3-N16R8, 7" 800x480 RGB LCD, GT911
  touch). Full dashboard UI, WiFi debug page, setup wizard, PIN lock.
- **CYD** (`BOARD_CYD_28_RESISTIVE` / `BOARD_CYD_28_CAPACITIVE` /
  `BOARD_CYD_24_RESISTIVE`): cheap secondary cockpit displays - plain
  ESP32 dev boards with a small ILI9341 SPI screen (320x240 landscape,
  resistive XPT2046 or capacitive GT911 touch depending on variant).
  Compact glow/start/silence-only screen, unconditionally
  `DISPLAY_ROLE_SECONDARY` (no ignition authority, no WiFi, no PIN, no
  wizard - see `create_ui_cyd()`/`ui_tick_cyd()` in `engine_display.ino`).

It will also talk CAN to a future CTRL board on the engine (relays,
senders, audio alarms). Development happens on the bench with fake
engines via a WiFi debug page (HELM only); no transceivers are wired yet.

**To switch targets**: edit the `TARGET_BOARD` line in `board_select.h`,
then reflash. Everything else (pin maps, display/touch driver selection,
CAN pins, which screen gets built) follows from that one line.

## CYD pin maps — community/commonly-documented defaults, VERIFY AGAINST YOUR ACTUAL BOARD BEFORE FLASHING

CYD clone pinouts vary by manufacturing batch even under the same product
name. These are starting points, set in `esp_panel_board_custom_conf.h`
and `engine_display.ino`'s `PIN_CAN_TX`/`PIN_CAN_RX` - not a guarantee:

| Signal | 2.8" resistive | 2.8" capacitive | 2.4" resistive |
|---|---|---|---|
| TFT SPI: SCK / MOSI / MISO | 14 / 13 / 12 | 14 / 13 / 12 | 14 / 13 / 12 |
| TFT: CS / DC | 15 / 2 | 15 / 2 | 15 / 2 |
| Backlight | 21 | 21 | **27** |
| Touch | XPT2046 (SPI, separate bus) | GT911 (I2C) | XPT2046 (SPI, separate bus) |
| Touch pins | CLK25 MOSI32 MISO39 CS33 IRQ36 | SDA33 SCL32 INT21 RST25 | CLK25 MOSI32 MISO39 CS33 IRQ36 |
| CAN TX / RX | 22 / 34 | 22 / 34 | 22 / 35 |

If the image is mirrored/upside-down or touch doesn't line up with what's
drawn on first boot, adjust `ESP_PANEL_BOARD_LCD_MIRROR_X/Y` and the
matching `ESP_PANEL_BOARD_TOUCH_*` transform flags together in
`esp_panel_board_custom_conf.h` - see the comments there.

## Build / flash

**HELM:**
- Arduino IDE (or arduino-cli), board **ESP32S3 Dev Module**
- 16MB flash, partition "16M Flash (3MB APP/9.9MB FATFS)", **OPI PSRAM**
- **USB CDC On Boot: DISABLED** (serial goes to the CH340 port)
- **Erase All Flash Before Sketch Upload: DISABLED** (NVS holds wifi creds
  and engine selection)
- arduino-cli compile check:
  `arduino-cli compile --fqbn esp32:esp32:esp32s3 .` (add the board options
  above via --board-options if configured)

**CYD (any variant):**
- Arduino IDE (or arduino-cli), board **ESP32 Dev Module** (plain ESP32,
  not S3 - a different chip family from HELM)
- No PSRAM option (plain ESP32 has none)
- No "USB CDC On Boot" option - CYD serial always goes over an onboard
  CH340/CP2102 USB-UART bridge, not native USB, so there's no equivalent
  of the S3's stale-reload/manual-reset quirk documented for `can_sim`
- 4MB flash, **partition scheme "Huge APP (3MB No OTA/1MB SPIFFS)"** -
  same reason HELM needs its own non-default 16M scheme: this sketch's
  WiFi/WebServer/ESP32_Display_Panel footprint doesn't fit the default
  4MB/1.2MB-APP partition even after HELM-only UI code (icon assets, arc
  gauge, wizard, PIN lock - all gated behind `#if TARGET_BOARD ==
  BOARD_HELM_S3_800x480` in `engine_display.ino`) is compiled out of a
  CYD build. Verified: all 3 CYD variants compile to ~1.40MB (44% of the
  3MB app partition) with this scheme; the default scheme's 1.2MB APP
  partition is NOT enough (~1.40MB needed) - `arduino-cli` will fail with
  "text section exceeds available space" if you pick the default instead.
- arduino-cli compile check:
  `arduino-cli compile --fqbn esp32:esp32:esp32:FlashSize=4M,PartitionScheme=huge_app,PSRAM=disabled .`

**Both:**
- Libraries: ESP32_Display_Panel 1.x, LVGL **8.4.0** (v8 API, NOT v9),
  **ArduinoJson 7.x** (HELM-only OTA manifest parsing - see "OTA
  updates" below; harmless to have installed for a CYD build even though
  nothing in a CYD build actually uses it, OTA is HELM-only for now)
- lv_conf.h needs: LV_COLOR_DEPTH 16, LV_FONT_MONTSERRAT_28 = 1,
  LV_FONT_MONTSERRAT_48 = 1 (shared by all targets, no per-board changes)

## Files (tabs)

- `engine_display.ino` — main sketch (current feature level: "step 2"),
  builds for whichever `TARGET_BOARD` is picked
- `board_select.h` — the one file you edit to switch hardware target
- `can_protocol.h` — CAN protocol **v2** (engine-indexed), multi-engine and
  not tied to any one engine model despite the older `md2030_protocol.h`
  name it used to have. A v1 baseline copy exists in project archives;
  do not mix v1/v2 symbols.
- `icon_temp.c, icon_oil.c, icon_glow.c, icon_start.c, icon_stop.c` — LVGL
  image assets (TRUE_COLOR_ALPHA, 16-bit, swap=0), generated by
  Python/PIL scripts. HELM only - `create_ui_cyd()` uses plain text
  instead, no icon assets sized for a 320x240 screen exist yet.
- `lvgl_v8_port.h/.cpp` — from the ESP32_Display_Panel simple_port
  example, shared by all targets. Treat as vendored: don't modify without
  strong reason.
- `esp_panel_board_supported_conf.h` — selects the HELM board
  (`BOARD_VIEWE_UEDX80480070E_WB_A`) when `TARGET_BOARD ==
  BOARD_HELM_S3_800x480`, inert otherwise.
- `esp_panel_board_custom_conf.h` — hand-specifies the ILI9341/touch/
  backlight pin config for whichever CYD variant is selected; inert for
  HELM builds. See the pin table above.

## Hard constraints (violating these cost us days — do not "improve" them)

1. **HELM: free GPIOs are ONLY 10, 11, 12, 13, 17.** Everything else is
   RGB panel data/sync, touch I2C (18/19/20, RST 38), PSRAM (35-37), boot
   strap (0) or UART (43/44). CAN uses TX=17, RX=13; the direct-wire
   supplementary transport (see "Direct-wire supplementary transport"
   below) uses TX=10, RX=11 - leaving only **GPIO 12** genuinely spare
   now. This list is HELM-S3-specific - CYD boards are a different chip
   with their own, much smaller free-GPIO set (see the CYD pin table
   above; CAN uses TX=22, RX=34/35 there, unverified placeholders).
2. **Internal SRAM is the scarce resource.** WiFi, WebServer and LVGL
   widget structs can only live in internal heap; PSRAM cannot help them.
   The RGB bounce buffer is 2 DMA buffers in internal SRAM: at 48 lines it
   ate ~154KB and starved the heap to 3.5KB, silently breaking the WPA
   handshake, page serving, everything. It is now **16 lines** — keep the
   heap_report() instrumentation and the loop heartbeat; after boot,
   internal free should stay comfortably above ~60KB. If flicker returns
   under WiFi load, trade carefully (e.g. 24 lines) and re-measure.
3. **wifi_setup() is the proven-working recipe: do not restructure it.**
   Boot order display → CAN → blocking WiFi join, sequence
   mode(STA) → setSleep(false) → begin(). Non-blocking joins, STA→AP mode
   switches mid-retry (LoadProhibited crash), server.begin() before
   WiFi.mode() (null-semaphore assert), and touching WiFi APIs from the
   LVGL task (crash) have all been tried and all failed.
4. **ui_tick must stay change-guarded.** LVGL v8 invalidates on every
   HIDDEN-flag write and label set_text even when nothing changed; use the
   set_hidden() helper and last-value guards. Unguarded full-screen
   overlays redraw 800x480 5x/sec.
5. **CAN with no transceiver**: RX has a pullup, TX is single-shot
   (m.ss=1), and repeated bus-off auto-disables CAN until reboot. Keep all
   three or the error-interrupt storm degrades everything.
6. Buzzer/audio does NOT belong on this board (LEDC channel collides with
   backlight; audio is the CTRL board's job).

## Safety model (marine — this is not decorative)

- The MD2030 can ONLY be stopped mechanically - nothing on the CAN bus
  stops it, and CAP_STOP must never be set for it (not even the fake
  MD2030C on the debug page). Electronic stop is CAPABILITY-GATED
  (CAP_STOP, MSG_CMD_STOP) for a future CTRL board wired to an actual
  electric cutoff on an engine that supports it - never a blanket
  "stop" available for every engine. The START button doubles as STOP
  only when the selected engine announced CAP_STOP, only on the primary
  display, and only while running (see g_can_stop in ui_tick/start_cb).
  Same dead-man HELD discipline as glow/start below, not a single tap.
- Glow/start(/stop) are dead-man HELD messages: resent every
  HOLD_RESEND_MS (100ms) while pressed; CTRL drops the relay after
  HOLD_TIMEOUT_MS (300ms) of silence and enforces CRANK_MAX_MS (60s) for
  cranking. A held message means "finger on button NOW", never a latch.
- Ignition (and STOP) are primary-only authority - only HELM may command
  them (CTRL enforces by source-node byte). Glow/start/silence may come
  from HELM or CYD/secondary. Ignition itself is a latched
  desired-state, never timed out.
- Power model on the display: user_power is latched **per engine**
  (`user_power[MD_MAX_ENGINES]`, not a single shared flag - two engines
  behind one display need independent power states, or powering one on
  while viewing it would make every other engine you switch to also read
  as "powered on"); rpm >= 100 means engine running, which forces AND
  latches that engine's own power on (sticky until the user switches it
  off). START-held masks rpm from running detection.

## Autostart (glow -> crank -> retry, one tap)

A third button next to glow/start - single tap, not held - that drives
the *exact same* dead-man protocol automatically: hold glow for a
per-engine configured duration, then hold start for up to a configured
crank duration; if the engine doesn't catch (rpm never rises above
`RPM_RUNNING_MIN`), release and wait a configured duration, then re-crank
(no re-glow) up to a configured number of retries. **No protocol/CTRL
changes at all** - `autostart_tick()` just sets `glow_held`/`start_held`
(+ their `_press_ms` timestamps) exactly as `glow_cb`/`start_cb` do,
so `can_send_commands()`'s existing `HOLD_RESEND_MS` resend loop and the
CTRL board's own `HOLD_TIMEOUT_MS`/`CRANK_MAX_MS` enforcement are
unchanged and remain the real safety backstop.

- **Catch detection requires rpm to STAY above `RPM_RUNNING_MIN` for
  `AUTOSTART_CATCH_HOLD_MS` (500ms), not just clear it on one sample.**
  Confirmed on real hardware: cranking twitter (compression-stroke
  kicks, same phenomenon `can_sim` deliberately simulates via
  `SIM_CRANK_RPM_JITTER`) genuinely spikes rpm above the threshold well
  before an actual catch - a single-sample check released `start_held`
  on the first lucky twitch. A human doing this manually never notices
  because nobody releases START the instant the tach twitches; the
  sustained-hold requirement makes the automated version match that.
  `autostart_catch_since_ms` (0 = not currently above threshold) is
  explicitly reset to 0 at both `CRANKING` entry points (a fresh crank
  from `GLOWING`, or a retry from `WAITING`) so a stale timestamp from
  an earlier attempt can never look like an instant catch on the very
  next tick.
- One state machine only (`AUTOSTART_IDLE`/`GLOWING`/`CRANKING`/
  `WAITING`), not per-engine parallel runs - `glow_held`/`start_held`
  always target `sel_engine` in `bus_send()`, so `autostart_engine`
  records which engine a run is actually for. Switching `sel_engine` away
  from it mid-run, or losing power (`!g_power`), cancels the whole
  sequence immediately rather than silently commanding the wrong engine.
  Tapping the button again while a run is active also cancels it.
  - **`update_engine_state()`'s own auto-reselect (further down, "engine
    presence: auto-select") used to be able to trigger this cancel path
    by itself, with no user action at all** - confirmed on real hardware
    as the actual cause of "autostart just gives up partway through,
    inconsistently." That block exists to switch `sel_engine` to a
    still-broadcasting engine when the *viewed* one vanishes (see its own
    comment), but a target engine legitimately can blip off the bus for a
    few seconds mid-run (a brief ESP-NOW reconnect, not a real failure)
    while a *second* engine (e.g. `can_sim`'s default E0+E1 both enabled)
    stays visible - the reselect fired, `sel_engine` moved to that other
    engine, and `autostart_engine != sel_engine` then cancelled the run
    exactly as designed, even though the user never touched anything.
    Fixed by skipping the reselect whenever `autostart_state !=
    AUTOSTART_IDLE && autostart_engine == sel_engine` - the engine an
    active run targets is left alone so the run can either catch a
    reconnect within its own crank/wait timers or give up on its own
    schedule, not an unrelated bus hiccup. `engine_ageout_tick()` already
    zeroes `eng.rpm` via `reset_telemetry()` when the selected engine
    ages out, which `autostart_tick()` handles fine on its own (reads as
    "not caught yet" mid-crank, doesn't matter at all during
    GLOWING/WAITING) - the reselect was the only piece actually breaking
    things.
- Manual glow/start button presses are ignored while a run is active
  (`glow_cb`/`start_cb` both check `autostart_state == AUTOSTART_IDLE`) -
  otherwise a stray touch could release a flag autostart is holding
  without the state machine knowing.
- Retry behavior: glow runs once at the very start of the whole sequence;
  each retry after a failed crank re-cranks only, never re-glows.
- Settings (glow/crank/wait seconds + retry count) are **per-engine**,
  NVS keys `asG%d`/`asC%d`/`asW%d`/`asR%d`, editable from Settings ->
  Autostart Settings (opens as a second modal - the main Settings window
  had no room left for 4 more rows). Crank seconds default to 30 and are
  clamped to `CRANK_MAX_MS`/1000 (60) in the UI - the CTRL board's own
  deadman cuts cranking there regardless of what's configured higher.
- **CYD gets the button and can trigger a run, but uses the compiled-in
  default timings** (`as_glow_s[]` etc.'s array initializers) - it has no
  Settings/wizard/PIN flow at all (see `create_ui_cyd()`), and a full
  per-engine config UI on a 320x240 screen was judged disproportionate
  scope for a secondary bench display. Easy to revisit if wanted later.
- `can_sim` has a matching "Force fail to start" per-engine debug-page
  toggle for bench-testing the retry path against an engine that never
  catches - see `can_sim/CLAUDE.md`.

## OTA updates (HELM self-update + HELM-triggered remote updates)

HELM checks a hardcoded URL (`OTA_MANIFEST_URL`: `ota/manifest.json` in
the OpenBoat repo on GitHub) for a small JSON manifest and can
download+flash its own new firmware (Phase 1), and can
also push a remote device (currently `can_sim` only) through the same
join-WiFi-and-flash cycle by handing it credentials + a download URL
over CAN/ESP-NOW (Phase 2). CYD is out of scope entirely - its
`huge_app` partition scheme has only one app partition (the Arduino
IDE's own menu label literally says "No OTA"), unlike HELM's
`app3M_fat9M_16MB` scheme, which already has the `ota_0`/`ota_1` pair
`Update.h` needs with no partition-table change.

- **Version scheme**: `FW_BUILD` (top of `engine_display.ino`) is a
  plain monotonic integer, bumped by hand every release - not semver.
  Comparison is just `remote_build > FW_BUILD`.
- **Manifest**: JSON at `OTA_MANIFEST_URL`, one entry per device type and
  then one per **hardware variant** -
  `{"helm":{"viewe7":{"build":N,"url":"...","md5":"..."}},
  "can_sim":{"s3zero":{...}, ...}}`. HELM looks itself up as
  `HELM_HW_KEY` ("viewe7"); a remote board is looked up by the `hw_id` it
  reports in `ANNOUNCE[7]` (`md_hw_key()` in `can_protocol.h` turns the id
  into the key; id 0 = a first-generation board that reports nothing,
  treated as `s3zero`). Different boards therefore get different images.
  The images are GitHub Release assets; `tools/release.sh` builds, uploads
  and writes the manifest (see `../tools/README.md`). `md5` is a 32-hex-
  char checksum of that entry's `.bin` - optional (an older or hand-edited
  manifest without one just skips verification, doesn't fail) but always
  present from the publish script. Parsed with **ArduinoJson
  7.x**'s unified `JsonDocument` API (no manual buffer sizing, unlike
  ArduinoJson 6.x's `StaticJsonDocument<N>`). `check_for_update_tick()`
  fetches it once per boot and every `UPDATE_CHECK_INTERVAL_MS` (~24h)
  after that, using a locally-scoped `HTTPClient` with a
  `WiFiClientSecure` (not persistent file-scope objects - matches the
  internal-SRAM-scarcity constraint above) over **HTTPS** (GitHub), with
  `setInsecure()` - the certificate chain is not verified, same trade as
  the other OpenBoat firmwares; the md5 guards the download. A plain
  `http://` manifest/URL still works (a local test server via a
  git-ignored `local_config.h`, see `local_config.example.h`). The TLS
  handshake needs a chunk of internal heap that LVGL also wants - **not
  yet tried on the real HELM**, watch the `OTA:` serial lines and
  `heap_report()` on the first check. A **"Check for Updates" button**
  (off_overlay, below the Update Available button, always visible) lets
  the operator bypass the ~24h wait - `check_update_now_cb()` just sets
  `g_force_update_check`, which `check_for_update_tick()` (loop task)
  consumes on its next pass to skip the interval gate; a glow/start hold
  defers the check rather than dropping it (same reasoning as the
  automatic check), but no WiFi connection clears the flag immediately
  so the button doesn't hang. `lbl_check_status` (change-guarded off the
  flag's true->false transition in `ui_tick()`) shows "Checking...",
  then a real result every time via `g_last_check_result` (set at every
  exit point of `check_for_update_tick()`, success or failure) - "No
  WiFi connection", "Update check failed" (HTTP/parse/malformed-manifest
  error), "No updates available", or "Update available!" - deliberately
  never hidden just because the Update Available button also appeared; a
  tap on this button should always get a visible answer, not silence.
  Like `remote_ota_modal_tick()`'s identical fix, this label update is
  followed by an explicit `lv_refr_now(NULL)` - confirmed on real
  hardware that without it, "Checking..." could get stuck on screen
  forever even though the label's text had genuinely already changed at
  the LVGL-object level (same RGB-panel-under-load root cause).
  - **A second, independent cause of the same "stuck on Checking..."
    symptom, found after the above fix was already live**: `ui_tick()`
    used to detect a finished check by edge-detecting `g_force_update_
    check`'s true->false transition - but `check_for_update_tick()` runs
    on `loop()` (roughly every 2-5ms) while `ui_tick()` only samples that
    flag once per 200ms timer tick. Confirmed on real hardware: the
    entire true->false pulse can complete well within one 200ms gap,
    meaning `ui_tick()`'s poll can genuinely never observe the flag as
    true at all - it never arms its own pending-latch, so the "Checking..."
    text set synchronously by `check_update_now_cb()` is never replaced,
    regardless of `lv_refr_now()`. Fixed by replacing the boolean
    edge-detection with a pair of monotonic counters:
    `g_update_check_request_id` (bumped once per button tap) and
    `g_update_check_done_id` (set equal to the request id that was
    current for a given run, at every one of `check_for_update_tick()`'s
    *forced* exit points - no-WiFi, every fetch/parse-failure return, and
    the success path - but deliberately NOT the glow/start-held defer,
    which isn't actually finished yet). `ui_tick()` just compares the two
    IDs for inequality each poll - an integer that changed once and holds
    still can never be missed by a slower poller, unlike a transient
    pulse.
- **Deliberately skipped whenever `glow_held`/`start_held` is true, OR
  `autostart_state != AUTOSTART_IDLE`**: the manifest fetch is a blocking
  HTTP GET on the same `loop()` task that resends `MSG_CMD_GLOW_HELD`/
  `MSG_CMD_START_HELD` every `HOLD_RESEND_MS` - stalling that mid-crank
  could abort a real start attempt. Deferred by one tick and retried
  later; not time-critical. **The `autostart_state` half of this guard
  was added after the held-flags-only version turned out to be
  insufficient, confirmed on real hardware**: autostart's `WAITING` phase
  (between a failed crank and the next retry) has both held flags false,
  so a check could start there - `http.begin()`/`GET()` has no way to
  abort once in flight, so if it's still running when `autostart_tick()`
  (a separate task/timer) flips `start_held` back true for the retry,
  `can_send_commands()` stays stalled behind the fetch and the retry's
  first command frame goes out late or not at all, tripping `can_sim`/
  CTRL's own `HOLD_TIMEOUT_MS` dead-man logic almost immediately - reads
  as "autostart gives up too early" with no error shown anywhere. A long
  enough stall (slow/unreachable manifest host) could also trip the loop
  task's watchdog and reboot the board outright. Checking the held flags
  alone only stopped a check from *starting* mid-hold; it did nothing for
  the gap between autostart phases, which is exactly what `autostart_
  state != AUTOSTART_IDLE` closes - deferred for the whole sequence, not
  just the instants a relay is actually held.
- **Notification vs. the actual safety gate are two different checks**:
  `g_helm_update_available` (set by the tick above) only controls
  whether the low-key "Update Available" button shows on `off_overlay`
  (never over the live dashboard - that button doesn't exist anywhere
  else). Tapping it runs the *real* check, `any_engine_active()` -
  every engine slot, not just the selected one (`engines[i].ign_on` is
  the best available signal for a non-selected engine, since live rpm
  is only tracked for `sel_engine`) - and refuses outright if anything's
  active. Only if that passes does the explicit warning screen appear
  ("only update in a secure environment..."), requiring a tap to
  proceed - see `ota_update_available_cb()`.
- **Download**: `ota_start_download_cb()` uses `HTTPUpdate.h`'s
  `httpUpdate.update(client, url)` convenience wrapper (not hand-rolled
  `Update.h` calls) with `rebootOnUpdate(false)` so a success shows a
  confirmation message before `ESP.restart()`, rather than an
  unexplained reboot.
  - **Checksum verification**: `httpUpdate.setMD5sum(g_helm_update_md5)`
    is called right before `httpUpdate.update()` whenever the manifest
    had an `md5` for this entry - this is HTTPUpdate's own built-in
    mechanism (`HTTPUpdate.cpp`'s `runUpdate()` passes it straight to
    `Update.setMD5()`), so `Update.end()` itself rejects a mismatched
    image rather than this file re-implementing hashing by hand. A
    mismatch surfaces through the existing generic failure path
    (`httpUpdate.getLastErrorString()`, e.g. "Wrong MD5") - no separate
    UI needed. Skipped (not failed) if the manifest had no `md5`.
  - **The RGB panel visibly corrupts during the flash write unless you
    actively work around it - confirmed on real hardware, don't "fix"
    this back to the simpler original design.** The original assumption
    ("LVGL renders on its own FreeRTOS task, so releasing
    `lvgl_port_lock()` before the blocking call keeps the display
    working") was wrong on two levels: (1) releasing the lock lets
    LVGL's port task keep calling `lv_timer_handler()` throughout the
    download, which reads flash-mapped font/icon data on every frame,
    colliding with `Update.h`'s repeated flash write/erase calls (each
    one disables the flash cache and briefly halts whichever core isn't
    writing); (2) even with the lock held the whole time (which does
    stop LVGL's own task from rendering), the panel *still* corrupted,
    because the RGB bus's bounce buffer (see `RGB_BOUNCE_LINES` in
    `setup()`) is refilled on a tight schedule by the display driver
    itself, and that refill starves during the same cache-disabled/
    core-halted windows regardless of what LVGL is doing - a lower-level
    DMA problem the LVGL lock can't reach. There's no clean fix short of
    touching the RGB driver directly, so instead: `g_board->
    getBacklight()->off()` right before `httpUpdate.update()`, `->on()`
    right after it returns. The panel is still corrupting internally the
    whole time, it's just invisible with the backlight off - the message
    says "screen will go blank until it restarts" rather than showing
    download progress. `g_board` is the `Board*` from `setup()`, saved
    globally specifically so this callback can reach it.
  - `lvgl_port_lock()` is still held for the entire `httpUpdate.update()`
    call (not just widget creation) even though it wasn't sufficient
    alone - it's still correct and avoids wasted contention. `lv_refr_now
    (NULL)` forces the "Performing update..." message to actually flush
    to the screen once before the lock is monopolized and the backlight
    goes off - otherwise it would never render at all, since the only
    thing that flushes it (the port's background task) is what's being
    held off.
- **Rollback safety**: `setup()` calls
  `esp_ota_mark_app_valid_cancel_rollback()` once board/display/WiFi/
  ESP-NOW init have all already succeeded without crashing - on a
  rollback-enabled partition table (which HELM's already is), an image
  that's never marked valid before the next reboot gets automatically
  rolled back to the previous one, so a genuinely broken update can't
  brick the board. Whether the installed ESP32 Arduino core actually has
  rollback compiled in wasn't independently verified from source alone -
  this call is a harmless no-op either way, but don't treat it as a
  guarantee without testing an actual bad-image rollback on real
  hardware.

### Phase 2: HELM-triggered remote updates (`can_sim` for now)

Every `ANNOUNCE` frame now carries the sender's `fw_build`
(`engine_info_t.fw_build`, parsed in `can_handle_rx()`) - HELM already
receives this from every enrolled engine at 1Hz, no new polling needed.
`check_for_update_tick()` (see Phase 1 above) additionally parses a
`"can_sim"` entry from the same manifest fetch into
`g_can_sim_update_build`/`g_can_sim_update_url`. `any_remote_update_
available()` compares every present engine's `fw_build` against that
cached build number - a match (nonzero and behind) is what makes
`btn_update_available` appear, same button/gating as Phase 1.

- **No per-device type field yet**: every remote `fw_build` is compared
  against the manifest's `"can_sim"` entry unconditionally, since
  `can_sim` is the only OTA-capable remote board that exists. A future
  real CTRL board with its own independent firmware will need
  `ANNOUNCE` (or a new field) to say which manifest entry it belongs to
  - not built yet, tracked here as a known gap.
- **Device-list modal**: tapping "Update Available" runs the same
  `any_engine_active()` safety gate and warning screen as Phase 1, but
  the "Continue" button is now a scrollable list (`ota_update_available_
  cb()`) - one row per updatable target (HELM itself, if
  `g_helm_update_available`, plus any present engine whose `fw_build` is
  behind). Each row's own "Update" button starts that device's update
  immediately - no second per-device confirm screen, since the warning
  above already covers every target in the list.
- **Sending the command**: `ota_send_start()` chunks `"<ssid>\0<password>
  \0<url>\0<md5>\0"` into `MSG_OTA_START(e)` frames exactly like
  `can_sim.ino`'s `sim_engine_send_name()` chunks a name (same
  `total_len`/`chunk_idx` framing, see `can_protocol.h`'s comment) -
  `OTA_SSID_MAXLEN`(32)/`OTA_PASS_MAXLEN`(64)/`OTA_MD5_MAXLEN`(32)/
  `OTA_URL_MAXLEN`(118) are fixed field caps that sum to exactly
  `OTA_START_MAX_LEN` (250) including their 4 NUL terminators, so the
  packed length can never overflow. `md5` comes from
  `g_can_sim_update_md5` (parsed from the manifest's `"can_sim"` entry,
  same as HELM's own self-update checksum) and may be an empty string if
  the manifest had none - `can_sim.ino`'s `ota_tick()` skips verification
  in that case rather than failing. Credentials come
  from `prefs.getString("ssid"/"pass", "")` (HELM's own saved WiFi
  creds, read fresh at send time, not cached in a global) - **HELM must
  itself be on working WiFi** (`WiFi.status() == WL_CONNECTED`, checked
  again right before sending) or there's nothing to hand off. Routed
  through `bus_send()` like everything else - CAN or ESP-NOW depending
  on that engine's live transport, and since `MSG_OTA_START` is in
  `md_is_unicast_command()`'s bucket, it gets AES encryption on the
  ESP-NOW path (plain, unencrypted on CAN - an accepted tradeoff for
  CAN-only devices, same reasoning as the other unicast commands).
- **Waiting for ACK**: `g_remote_ota_state` (`REMOTE_OTA_WAIT_ACK` ->
  `REMOTE_OTA_ACKED`/`REMOTE_OTA_TIMEOUT`) is a plain enum flip, not a
  direct widget mutation - `can_handle_rx()`'s new `MSG_OTA_ACK` branch
  can run off the LVGL task (loop()'s `can_poll()`, or an ESP-NOW receive
  callback) so it only ever sets the enum, same handoff shape as
  `glow_held`/`start_held`. `remote_ota_modal_tick()`, called every
  `ui_tick()` cycle (200ms, so it's genuinely running on the LVGL task),
  notices the state moved off `WAIT_ACK` (either the ACK arrived, or
  `OTA_ACK_TIMEOUT_MS` (30s - was 8s, too tight in practice, see that
  constant's own comment) elapsed) and repaints the modal exactly once
  - "acknowledged, updating..." or "no response" with **Close / Wait
  Longer / Retry**. Wait Longer (`ota_wait_longer_remote_cb()`)
  deliberately does NOT re-send `MSG_OTA_START` - it just pushes
  `g_remote_ota_sent_ms` out another `OTA_ACK_TIMEOUT_MS` and goes back
  to `WAIT_ACK`, for the case where the original burst likely did arrive
  and the target's already mid-update (joining WiFi, downloading,
  flashing) by the time the timeout fires - re-sending in that case is
  redundant at best, and could land on a device no longer listening the
  same way mid-flash. Retry (`ota_retry_remote_cb()`) is the actual
  re-send, for when the burst genuinely didn't arrive at all.
  Closing the window (`ota_close_cb()`) always resets
  `g_remote_ota_state` to idle, so a late/stray ACK after the user gave
  up can't reopen or repaint a dismissed window.
- **`REMOTE_OTA_CONFIRMED`**: build-number confirmation, independent of
  whether `MSG_OTA_ACK` ever arrived at all. Confirmed on real hardware
  that `can_sim` can genuinely complete a remote update (proven by it
  broadcasting a fresh `ENROLL_REQUEST`, which only happens after a
  firmware reboot) while HELM's UI still shows "no response" - the ACK
  is a one-shot unicast (see `can_sim.ino`'s `ota_tick()` comment on why
  it now sends 3 copies, which reduces but doesn't eliminate the loss
  risk). `remote_ota_modal_tick()` now keeps monitoring (doesn't
  early-return) in both `WAIT_ACK` and `TIMEOUT` states, and on every
  tick checks whether `g_remote_ota_engine`'s own `fw_build` (from its
  regular `ANNOUNCE` broadcasts) has caught up to `g_can_sim_update_
  build` (the cached manifest value) - if so, forces the state to
  `CONFIRMED` regardless of the ACK/timeout state, and repaints with a
  single Close button ("...is back online running build N - the update
  succeeded."). Deliberately doesn't mention the missing ACK - the
  operator only cares that the device is confirmed back on the right
  build, not the internal reason HELM took the slower confirmation path.
  This check runs even after the
  Retry/Wait Longer screen is already showing, since the full WiFi-join+
  download+flash+reboot cycle can easily outlast `OTA_ACK_TIMEOUT_MS`
  even on a healthy link. A `static remote_ota_state_t last_drawn`
  inside the function (not the same variable as `g_remote_ota_state`
  itself) tracks what's actually been painted, since the state can now
  transition twice in one tracked run (`WAIT_ACK` -> `TIMEOUT` ->
  `CONFIRMED`) where the original design only ever handled one
  transition.
- **Not yet done**: no live progress feedback once the target actually
  starts downloading (HELM has no visibility between the ACK and the
  build-number confirmation above - the device goes quiet on the bus
  while it joins WiFi/downloads/flashes/reboots, matching `can_sim.ino`'s
  `ota_tick()` design).

## Remote WiFi join (debug/admin trigger)

Two entry points, one shared implementation (`wifi_join_all_devices()`):
hands HELM's own saved WiFi credentials to every currently-present engine
via `MSG_WIFI_JOIN(e)` (`wifi_join_send()`) - added so a bench `can_sim`
board's own debug page becomes reachable without a USB cable, without
needing OTA/firmware machinery for something that's really just "join
this network and remember it."

- **WiFi debug page**, "Remote WiFi Join" section: **"Join & Stay on WiFi
  (All Devices)"** button (`handle_wifi_join_all()`, `/wifijoinall`).
- **Touchscreen Settings dialog** (primary only): **"Join All WiFi"**
  button (`wifi_join_all_cb()`) alongside "Wireless Pairing"/"Autostart
  Settings" - added so this doesn't require already being on the network
  to reach the debug page in the first place. Button + result share one
  row (button left half, result label `lbl_wifi_join_all` right half)
  rather than stacking, to keep the dialog's total height from creeping
  back toward the ~472px that used to cut Factory Reset/Close off on real
  hardware (see `settings_cb()`'s own comment on `SETTINGS_BTN_H`) - the
  result strings are kept short (`wifi_join_all_devices()`'s own comment)
  specifically so they fit that narrower column. Synchronous - just
  writing a handful of bus frames - so the result shows immediately, no
  tick-based change-guard needed the way the async OTA-check flow needs.
- **Reaches a present engine over whichever transport it's actually on**
  - CAN, ESP-NOW, or the direct-wire link (see "Direct-wire supplementary
  transport" above) - `bus_send()`'s existing per-engine routing handles
  all three identically, so neither entry point needs to know or care
  which transport wins.

- **Reuses `can_sim`'s existing serial `WIFI:<ssid>,<password>` mechanism
  on the receiving end**, just triggered wirelessly instead of typed over
  USB - see `can_sim/CLAUDE.md`'s `save_wifi_and_reboot()`. The join is
  persisted to the receiver's own NVS and it reboots to actually connect,
  so it stays on that network across future reboots too, not just for
  the current session - satisfies "join and stay on it", as opposed to a
  transient `WiFi.begin()` that would be lost on the next reboot.
- **Same chunking/gating shape as `MSG_OTA_START`, deliberately much
  simpler payload**: just `"<ssid>\0<password>\0"` (`wifi_join_send()`,
  `WIFI_JOIN_CHUNK_BYTES`/`WIFI_JOIN_MAX_LEN` in `can_protocol.h`) - no
  url/md5, since this isn't a firmware flash. In `md_is_unicast_command()`'s
  bucket like `MSG_OTA_START`, for the same reason: a WiFi password
  deserves AES protection when routed over ESP-NOW.
- **Reaches every physical board with at least one present engine slot**
  - loops `engines[]`, sends to each present index. A board enrolled
  ONLY as an alarmer (no engine slots) isn't reachable this way; not a
  concern for `can_sim` today (always enrolls at least one engine), noted
  as a known gap alongside the OTA section's "no per-device-type field
  yet" gap.
- **HELM must itself be on working WiFi** (checked right before sending,
  same reasoning as the OTA remote-trigger path) - nothing to hand off
  otherwise. Both entry points show a plain text result ("Sent to N
  device(s), rebooting", "No saved WiFi credentials", etc.) rather than a
  modal, since this is a fire-and-forget debug action with no ACK/
  confirmation loop like OTA has - the operator just watches for the
  target board to reappear on the bus (or reach its own debug page) after
  its reboot.

## Protocol v3 summary (can_protocol.h is authoritative)

Engine-indexed 11-bit IDs, e = 0..3: commands 0x080/0x090/0x0A0/0x0B0/0x0C0
+ e (ignition, glow-held, start-held, alarm-silence, stop; payload
[value, source node]); telemetry 0x100+e (rpm u16LE, temp x10 i16, oil
x100 u16, flags), hours 0x110+e (u32 x10), ANNOUNCE 0x120+e ([e, caps
u16LE, type, proto_ver, fw_build u16LE]) at 1Hz, name 0x130+e (chunked,
see below), OTA start 0x140+e (chunked, HELM->device, see "OTA updates"
above), OTA ack 0x150+e (device->HELM, one frame), and WiFi join 0x160+e
(chunked, HELM->device, see "Remote WiFi join" below). Displays
discover engines from ANNOUNCE (10s ageout), auto-select the first present
engine, show NO ENGINE DETECTED when the bus is empty, and drive widget
visibility from the caps bits - CAP_STOP included, which gates whether
START can ever turn into a STOP control (see Safety model above). Two more
alarm caps beyond temp/press exist now: CAP_CHARGE_ALARM (alternator not
charging) and CAP_WATER_ALARM (water-in-fuel sensor) - both wired at the
protocol/data level (engine_data_t could grow the two flags the same way
temp/press did) but **not yet surfaced in the HELM UI** (no icons/widgets
yet - that's real estate/pixel-collision work like the rest of this
screen, deliberately deferred rather than half-done).

## Bus enrollment (dynamic addressing)

HELM is always the bus MASTER. Every other node - engine CTRL boards, the
alarmer - is a SLAVE that gets its engine/alarmer index handed out at
runtime instead of hardcoded/jumpered on that board, identified by its
MAC address (ESP32s all have a unique one). An unenrolled node broadcasts
MSG_ENROLL_REQUEST ([MAC, node_type]) every ENROLL_RETRY_MS; HELM looks
the MAC up in an enrollment table persisted in NVS (so a rebooted/
replaced board gets the SAME index back, not a new one - keyed "encM0".."encM3"
for engines, "almM0"/"almM1" for the alarmer) and broadcasts
MSG_ENROLL_ASSIGN ([MAC, assigned_id]) - every node hears it and only the
matching MAC acts on it. No free slot -> ASSIGNED_ID_NONE (0xFF), and the
node retries slower rather than spamming the bus. Only HELM ever sends
ENROLL_ASSIGN; a secondary display never enrolls devices (bus listener
only, same as its lack of ignition/stop authority elsewhere). Once
enrolled, an engine controller also sends its long human-readable name
(MSG_ENGINE_NAME, chunked - "Volvo Penta MD2030C" doesn't fit one 8-byte
frame) - purely cosmetic, ENGTYPE_* is still what gates capability/UI
behavior. Full handshake documented in can_protocol.h's ENROLLMENT
comment - that file is authoritative, this is just the summary.

## ESP-NOW pairing + transport (HELM only)

HELM runs WiFi (STA/AP, as above) and ESP-NOW simultaneously - they share
the same radio and must be on the same channel, which is automatic since
`espnow_setup()` is only called after `wifi_setup()` has already settled
into its final STA-joined or AP-fallback state (see `setup()`).

A paired peer's MAC is a trust decision, not a routing decision by
itself - once paired, that peer's actual protocol traffic (glow/start/
ignition/stop, telemetry, discovery, enrollment) really does flow over
ESP-NOW, exactly the same `can_protocol.h` messages CAN carries, tunneled
through a small generic envelope (`espnow_bus.h`) rather than a
redesigned protocol. See "Bus transport routing" below for the shape.

- HELM is always the pairing **acceptor**. Normally it ignores every
  ESP-NOW pairing request. Tapping **"Wireless Pairing"** in Settings
  (primary only), or **"Enter Pairing Mode"** on the WiFi debug page
  (`/esppair`, no touchscreen needed), opens a 60s window
  (`PAIR_WINDOW_MS`, `espnow_pairing.h`) during which a new, never-before-seen MAC gets
  added to a persisted allowlist (NVS keys `espM0`..`espM5`/`espL0`..`espL5`,
  MAC + per-peer key, same fixed-slot + hex-encoding pattern as the CAN
  enrollment table) and receives a `PAIR_MSG_ACK`. Outside that window,
  unrecognized MACs are silently ignored - a MAC already on the allowlist
  stays paired forever without needing the window reopened.
  The Settings dialog's "Wireless Pairing" button label switches to
  "Wireless Pairing (open)" **immediately on tap**, not just when the
  dialog is closed and reopened - `lbl_wireless_pairing` is change-
  guarded off `g_pairing_mode` itself in `ui_tick()` (not set inline in
  `wireless_pairing_cb()`), since `g_pairing_mode` can also flip true
  from the web debug page's `/esppair` handler and back to false from
  `pairing_mode_tick()`'s auto-close - one LVGL-task change-guard covers
  all three trigger sources instead of duplicating the label update.
- Other boards (`can_sim`/future CTRL boards) are pairing **requesters**:
  broadcast `PAIR_MSG_REQUEST` fast for the first 60s after boot, then
  back off to a slow retry forever (mirrors the CAN
  `ENROLL_RETRY_MS`/`ENROLL_RETRY_SLOW_MS` two-speed shape). Once a
  requester receives its `PAIR_MSG_ACK`, it never broadcasts again on its
  own - only an explicit re-arm trigger (a serial `REPAIR` command on
  `can_sim`) starts it broadcasting again.

### AES pairing (per-peer encryption)

Every paired peer is encrypted, not just allowlisted. When HELM accepts
a new MAC it generates a fresh random 16-byte key (`esp_random()`, the
peer's "Local Master Key" / LMK) and sends it back inside the
`PAIR_MSG_ACK` - both sides then register that peer with `encrypt=true`
+ that key, so ESP-NOW transparently encrypts/decrypts all traffic
between them from then on. A message with the right `fleet_id`/format
but not encrypted with the right key gets silently dropped by the radio.

- **The ACK itself is necessarily unencrypted** - neither side has the
  key yet at that exact instant, that message *is* the key exchange.
  HELM registers a new peer `encrypt=false` first, sends the ACK, then
  upgrades it to `encrypt=true` ~200ms later (`espnow_upgrade_tick()`,
  `PAIR_UPGRADE_DELAY_MS`) - not immediately after `esp_now_send()`,
  since `esp_now_send()` is asynchronous and switching the peer's
  encryption state before the plaintext ACK has actually gone out over
  the air would race it. This is a **proportionate**, not bulletproof,
  security boundary: an attacker would need to be actively sniffing
  during the few-second window right when you deliberately trigger a new
  pairing to capture a key - see `espnow_pairing.h`'s comment for the
  full reasoning.
- **`ESPNOW_PMK`** (`espnow_pairing.h`) is the device-wide key used
  locally to protect each peer's LMK in the radio's crypto engine
  (`esp_now_set_pmk()`) - unlike `FLEET_ID` and each peer's LMK, it does
  NOT need to match between devices for pairing to work, but edit it to
  your own value alongside `FLEET_ID` anyway; the shipped value is a
  placeholder.
- **6-peer hard cap**: `ESP_NOW_MAX_ENCRYPT_PEER_NUM` (esp_now.h) limits
  any one board to 6 simultaneously-encrypted peers, so `ESPNOW_MAX_PAIRED`
  is 6, not the 8 the infrastructure-only version used - comfortably
  above `MD_MAX_ENGINES` (4) + an alarmer + one secondary display.
- **Upgrading from the plaintext infrastructure-only version**: old
  allowlist entries have no saved key (the message struct grew a `lmk`
  field, old and new peers aren't compatible). Use **Clear ESP-NOW
  Pairings** on HELM and **Clear Pairing**/`REPAIR` on each other board
  once, then re-pair - `can_sim`'s `espnow_setup()` already detects a
  "paired but no saved key" state on boot and auto re-arms itself rather
  than getting stuck, but HELM's allowlist needs the manual clear since
  it has no equivalent single-peer auto-recovery.
- The WiFi debug page's **"Clear ESP-NOW Pairings"** button (`/espclear`)
  wipes HELM's allowlist (in-memory + NVS `espM0`..`espM5`/`espL0`..`espL5`)
  and its ESP-NOW peer table. This is one-sided: it does not reach out
  and tell a previously-paired module it's been forgotten - that
  module's own `can_sim`-style `REPAIR`/"Clear Pairing" is what re-arms
  broadcasting on its end. The debug page also shows a live paired-peer
  count and pairing-window countdown (`/espstatus`), since the
  touchscreen gives no feedback once a peer accepts.
- **FLEET_ID** (`espnow_pairing.h`) must be identical across every board
  on your boat - edit it once to a value unique to your boat before
  flashing anything. A pairing request whose `fleet_id` doesn't match is
  ignored even during an open pairing window - this is what stops a
  neighboring boat's identical firmware (e.g. at a marina) from being
  able to pair with, or accidentally get paired by, your boards.
- **"CAN Enabled"** (Settings, primary only, default checked, NVS
  `can_dis` - inverted: unchecked means `g_can_disabled=true`): for a
  bench/dev unit with no CAN transceiver wired up - unchecking skips
  `can_setup()`'s TWAI init entirely and suppresses the flashing "NO
  LINK" indicator (both in the main dashboard and the Settings CAN
  status line), which would otherwise flash red forever with nothing
  wired up.
- **No automatic WiFi AP/join**: a board with no WiFi credentials
  configured (or a failed STA join) does NOT fall back to broadcasting
  its own AP - that's unrequested WiFi activity on a bench unit that's
  often deliberately run with zero WiFi at all. It stays in `WIFI_STA`
  mode with no active connection and pins the radio to a fixed channel
  (`ESPNOW_AP_FALLBACK_CHANNEL`, `espnow_pairing.h`) via
  `esp_wifi_set_channel()`, purely so ESP-NOW still works - every board
  doing this lands on the same channel and can pair with zero WiFi
  configuration anywhere. The debug page is simply unreachable in this
  state; `WIFI:<ssid>,<password>` over serial (or the wizard/Settings)
  is the only way a board starts actually using WiFi. A board that does
  join a real network uses whatever channel that router assigned
  instead - both boards still need to join the SAME real network for
  pairing to keep working once you do configure WiFi on both.
- **Not yet covered**: CYD (`engine_display.ino`'s non-HELM
  `TARGET_BOARD` targets) doesn't run ESP-NOW pairing at all - it would
  reuse the exact same requester logic `can_sim` has, but CYD never
  joins WiFi today (see "Files" above), so it has no channel to give
  ESP-NOW to inherit. Tracked as a follow-up, not done here. CYD (and
  `can_sim`/HELM with CAN enabled) still just gets plain CAN traffic,
  unaffected by any of this.
- `espnow_pairing.h` and `espnow_bus.h` are duplicated by hand into
  `can_sim/` (same "keep byte-identical" discipline as `can_protocol.h` -
  see that file's header comment).

### Bus transport routing

Raw CAN-frame access was already confined to two chokepoints
(`can_send()`/the `twai_receive()` loop in `can_poll()`) before this
existed, so transport selection lives entirely in those two places -
`can_handle_rx()` (the actual message dispatch) is untouched regardless
of which wire delivered a frame.

- **`bus_send()`** replaced `can_send()` (same signature, every call site
  just renamed). Splits messages into two buckets:
  - **Broadcast** (`ANNOUNCE`, `TELEM_PRIMARY`, `TELEM_HOURS`,
    `ENGINE_NAME`, `ALARM_SILENCE`, `ENROLL_REQUEST`, `ENROLL_ASSIGN`,
    `HB_HELM`, `HB_CYD`): dual-emits on every live transport - CAN if
    `!g_can_disabled && can_ok` (both, not `can_ok` alone - "CAN Enabled"
    in Settings/serial must actually stop CAN transmission at runtime,
    not just skip `can_setup()` on the next boot), ESP-NOW broadcast if
    any peer is paired - mirroring CAN's own bus-wide topology.
    `ALARM_SILENCE` is broadcast, not unicast, even though it's
    engine-indexed like the 4 commands below - a standalone alarmer
    *overhears* it the same way it overhears telemetry, not a
    point-to-point exchange.
  - **Unicast + encrypted** (`MSG_CMD_IGNITION`, `MSG_CMD_GLOW_HELD`,
    `MSG_CMD_START_HELD`, `MSG_CMD_STOP` only, via
    `md_is_unicast_command()` in `can_protocol.h`): routed to that
    specific engine's already-paired-and-encrypted MAC when its
    transport is ESP-NOW, never dual-emit - this is where the AES
    pairing work actually pays off, and it's deliberately only the
    authority/safety-relevant commands.
  - ESP-NOW broadcast frames **cannot be encrypted** (a real ESP-NOW
    constraint, not a choice) - the broadcast bucket's authenticity
    rests on the sender's radio MAC being on the pairing allowlist
    (`espnow_peer_known()`), weaker than the unicast bucket's AES
    authentication. This asymmetry is inherent to the design, not
    closed in this pass - see `can_protocol.h`'s AUTHORITY comment.
- **`espnow_on_recv()`**'s bus-frame branch builds a synthetic
  `twai_message_t` from the incoming envelope and feeds it to
  `can_handle_rx()` unchanged, then stamps `engine_slots[e].transport`/
  `espnow_peer_mac` - **on every inbound frame that names an engine, not
  just at enrollment**, so a stale transport tag can't survive HELM
  rebooting alone while a remote node keeps broadcasting without
  re-enrolling (self-heals within one `TELEM_PERIOD_MS`, 100ms).
- **Two MAC concepts, not one**: `derive_sim_mac()` gives `can_sim`'s 4
  fake engines + alarmer *synthetic* per-role MACs for CAN-protocol
  enrollment identity - but ESP-NOW pairing/encryption is tied to the
  *one real radio MAC*. Up to 5 `engine_slots`/`alarmer_slots` entries
  legitimately share the same `espnow_peer_mac` for one `can_sim` board -
  many-to-one, not a bug.
- **`can_send_commands()` has no `can_ok` guard, deliberately** - it used
  to `return` immediately whenever CAN was unhealthy, which silently
  dropped glow/start/ignition/stop/heartbeat entirely even with a fully
  working ESP-NOW link, since `bus_send()` never even got called to make
  its own per-message routing decision. Same class of bug as (and fixed
  alongside) `can_sim`'s `bus_send()` conflating CAN health with the
  user's CAN-enabled preference - see that file's CLAUDE.md.

### Direct-wire supplementary transport (`wired_bus.h`)

A third transport, alongside CAN and ESP-NOW, for when HELM and one
engine driver (currently just `can_sim`) are physically right next to
each other and running 3 wires is simpler than fitting a CAN transceiver
or relying on radio - e.g. a bench setup, or a future CTRL board mounted
right behind the panel. `wired_bus.h` (duplicated by hand into `can_sim/`,
same discipline as `can_protocol.h`/`espnow_bus.h`) is the full design
writeup; this is the HELM-specific half of it.

- **Wiring**: HELM `PIN_WIRED_TX` (GPIO 10) -> other board's wired RX,
  HELM `PIN_WIRED_RX` (GPIO 11) <- other board's wired TX, plus a common
  GND. `can_sim`'s side uses GPIO 1 (TX) / GPIO 2 (RX) - see that file's
  CLAUDE.md. Plain 3.3V UART logic levels, no transceiver chip, no
  termination - both ends are ESP32 family.
- **Strictly point-to-point** - exactly one other board can be on this
  wire at a time, unlike CAN's multi-drop bus or ESP-NOW's multi-peer
  model. This is why `wired_handle_bus_frame()`'s transport-stamping
  (mirrors `espnow_handle_bus_frame()`'s shape exactly) needs no peer
  address at all: `engine_slots[e].transport == BUS_TRANSPORT_WIRED`
  alone is enough for `bus_send()` to know where a future unicast command
  for that engine should go, since there's only ever one possible
  destination.
- **UART1** (`WiredSerial`, `HardwareSerial(1)`) - a completely separate
  peripheral from UART0, which this file's `LoggingSerial`/`#define
  Serial` machinery already wraps for the web-viewable serial log. No
  interaction between the two.
- **Always-on, not a toggle** - `wired_setup()` runs unconditionally in
  `setup()` (any `g_display_role`), matching `can_setup()`'s own "always
  try, note whether it worked" shape. A floating/disconnected RX pin
  reading noise just fails the frame checksum harmlessly and gets
  dropped by `wired_bus_tick()` - the same tolerance already accepted for
  CAN with no transceiver attached (hard constraint #5 above).
  `bus_send()`'s broadcast bucket dual-emits onto it unconditionally
  (`g_wired_ok`), alongside CAN and ESP-NOW - genuinely a *supplementary*
  transport, not a third mutually-exclusive alternative the way CAN vs.
  ESP-NOW is on `can_sim`'s sending side.
- **Reuses every existing `can_protocol.h` message completely unchanged**
  - `wired_bus.h`'s 13-byte frame (sync byte, u16 LE identifier, dlc,
  8 data bytes, XOR checksum) carries the exact same messages CAN and
  ESP-NOW do, via the same synthetic-`twai_message_t` trick
  `espnow_handle_bus_frame()` already uses - `wired_handle_bus_frame()`
  builds one and hands it straight to `can_handle_rx()`, so enrollment,
  telemetry, and commands all just work with zero protocol-level changes.
- **No encryption, no ACK/retry at this layer** - a direct wire is
  inherently more physically secure than radio (same reasoning CAN
  itself goes out in plaintext for), and the protocol's own dead-man/
  ENROLL resend loops are the real reliability backstop, not this
  transport. A corrupted/dropped byte just costs one lost frame -
  `wired_bus_tick()` resyncs at the next `WIRED_SYNC_BYTE` automatically.
- **Known gap**: only reaches a board with at least one enrolled *engine*
  slot (transport is stamped via `engine_slots[]`, same as ESP-NOW) - an
  alarmer-only board isn't addressable this way. Not a concern for
  `can_sim` today (always enrolls at least one engine).

## Bench workflow

- WIFI_SSID/WIFI_PASSWORD are blank in the sketch now — WiFi comes from
  NVS, set via the first-boot Initial Setup wizard, Settings, or
  `WIFI:<ssid>,<password>` over serial. If NVS has no creds (or a join
  fails) the panel does NOT fall back to an AP - see "No automatic WiFi
  AP/join" above - it stays ESP-NOW-only until you explicitly configure
  WiFi. Bench testing can still put real creds back in the #define
  block, but remember that overwrites NVS every boot — blank it again
  afterward.
- Debug page at the printed IP: sliders for rpm/temp/oil, hours input,
  alarm checkboxes, and 4 fake-engine toggles (E0 Volvo Penta MD2030C,
  E1 Yanmar 2YM15, E2 Yanmar 4JH80, E3 Nanni — all full caps for now).
  With no fake enabled the panel correctly sits on NO ENGINE DETECTED.
- **Serial Log panel** on the debug page (`/serial`, `handle_serial_log()`)
  mirrors this board's own serial console over HTTP - added because it
  runs headless on the boat, no USB cable to watch it with. `LoggingSerial`
  (top of `engine_display.ino`, right after the includes) subclasses
  `HardwareSerial` and overrides its two virtual `write()` methods to
  append into a 4KB ring buffer before forwarding to the real UART;
  `#define Serial g_log_serial` right after the class makes every
  existing `Serial.xxx(...)` call site in this file pick it up
  automatically, no other code changes needed. Scoped to this
  translation unit only (this `.ino` + its directly-`#include`d headers)
  - vendored library `.cpp` files (`lvgl_v8_port.cpp` etc.) are compiled
  separately and never see the `#define`. 8KB static total (4KB ring +
  a same-size scratch buffer in the handler to linearize it) - modest
  against this board's SRAM scarcity, but worth remembering it's there
  if headroom ever gets tight again (see the internal-SRAM hard
  constraint above). The panel polls `/serial` every second into a
  `readonly` `<textarea>` (not a `<pre>`, specifically so selecting text
  works the way it does in a normal text box) - **Pause** stops the poll
  entirely (`serialPaused` in the page's own JS) rather than just
  freezing the display, since a real-hardware test showed that trying to
  select text while it kept refreshing out from under you every second
  was effectively unusable.
- After ANY change, verify: WiFi joins, debug page serves repeatedly,
  heap heartbeat healthy, then feature behavior.
- Commit at every confirmed-working state. A working baseline had to be
  reconstructed from memory once; never again.

## Roadmap

1. ~~Remove hardcoded WiFi creds~~ done: Initial Setup wizard (first
   boot) + Settings -> Factory Reset. Wizard asks display role FIRST
   (g_display_role, NVS key "role") - this is the same "glow/start
   only, no ignition authority" deal as the planned CYD board below,
   just implemented on this same HELM sketch for a second identical
   display rather than a dedicated one. A secondary answer skips the
   WiFi step entirely and finishes right there: secondary panels never
   run wifi_setup() or the debug web server at all (see setup()/loop()).
   WiFi can also be set any time over serial: "WIFI:<ssid>,<password>"
   (ignored on secondary panels, which never join WiFi).
   Wizard also has a security-PIN step now, PRIMARY ONLY and skippable -
   secondary never gets it (doesn't wake up until an engine is switched
   on, so there's nothing on it worth locking). Optional lock screen at
   boot (numeric keypad), changeable later from Settings ("Change
   PIN"/"Set PIN", asks for the old one first if set). NVS key "pin",
   cleared by Factory Reset like everything else. Same
   trust model as the WiFi password already in NVS - stops guests from
   fumbling the controls, not a real security boundary.
2. CTRL board sketch (bare ESP32-S3 devkit): ANNOUNCE + telemetry +
   dead-man relay logic with authority enforcement + hours accumulation
   in NVS + audio alarms honoring ALARM_SILENCE
3. ~~CYD cockpit display (glow/start only, no ignition authority)~~ done:
   `board_select.h` + `create_ui_cyd()`/`ui_tick_cyd()`, see "CYD pin
   maps" above. Pin numbers are unverified community defaults - confirm
   against real hardware before trusting a bench test.
4. Real senders: tach from alternator W-terminal, VDO temp/pressure

## CYD over ESP-NOW (stage 1: receive-only, through HELM)

The CYD pairs with the HELM only and is a listener; HELM is the hub. Verified on real hardware
(HELM + C3 `can_sim` + CYD): the CYD receives the full engine data stream (~25 frames/s).

- **Pairing**: the CYD is a pairing REQUESTER like `can_sim` (`cyd_espnow_setup()` /
  `cyd_pairing_tick()`): it broadcasts `PAIR_MSG_REQUEST` with `node_type = NODE_TYPE_CYD` while
  hunting channels 1-13 for HELM; open HELM's pairing window (Settings -> Wireless Pairing, or `PAIR`
  on its serial console). HELM records each paired peer's node type (NVS `espT0..5`). Serial `REPAIR`
  on the CYD forgets HELM and re-pairs. HELM's MAC + key are kept in the CYD's NVS
  (`helm_mac`/`helm_lmk`/`paired`), the last good channel in `lastch`.
- **Data**: `espnow_relay_to_displays()` (called at the top of `can_handle_rx()`) forwards every
  ANNOUNCE / telemetry / hours / name frame HELM receives, over any transport, to each paired
  NODE_TYPE_CYD peer as an **encrypted unicast**. HELM's own heartbeat is also sent to them as an
  addressed unicast, in addition to the broadcast.
- **Contact detection - the trap**: at the close range of a bench, a broadcast sent on one channel
  is still decoded (faintly) from a neighbouring or even distant channel, but the encrypted unicasts
  are not. A CYD that counted broadcasts as "contact" sat on the wrong channel (it booted on channel 1)
  receiving ~10% of the traffic and never hunting. So the CYD (a) boots onto `lastch`, and (b)
  refreshes its contact timer only from frames **addressed to it** (`info->des_addr` is not the
  broadcast MAC). It may settle on a channel next to HELM's (it settled on 5 with HELM on 6); that
  works fine. The CYD's 5 s status line shows the radio channel and the frame rate; HELM's shows how
  many relays the radio acked (`radio acked N, no ack M`) - a high `no ack` means the CYD is not
  receiving.
- **Not done (stage 2)**: glow/start from the CYD over ESP-NOW. Those must reach the engine unit as
  encrypted unicasts with the CYD as the source; the plan is HELM relaying them (the CYD is paired with
  HELM only). The CYD's mute stays local by design - only HELM silences the bus-wide alarm.
- **can_sim needs `CANOFF`** (serial command, persisted) on a bench with no CAN transceiver, or it
  sends on CAN only and never on ESP-NOW.

## ESP-NOW security: the shared secret (replaces the pairing window)

There is no pairing window and no key on the air. Every board in the network holds the SAME secret, typed
in once as a passphrase and kept in that board's own flash (NVS key `fkey`). Nothing secret is compiled in:
the release images are public, so a compiled-in key would be public too (the old `FLEET_ID` / PMK placeholders
in this repo were exactly that, and are gone). All of it lives in `fleet_security.h` (identical copies in
`engine_display/` and `can_sim/`) with the message layouts in `espnow_pairing.h` / `espnow_bus.h`.

- **Setting it**: serial `KEY <passphrase>` (12+ characters), then the board restarts. `KEYSHOW` prints a
  *fingerprint* - an 8-hex-digit code that is the same on every board holding the same secret, so you can
  compare boards without showing the secret. `KEYCLEAR` forgets it. With no key, ESP-NOW stays OFF.
  Passphrase -> PBKDF2-HMAC-SHA256 (10000 rounds, fixed public salt) -> 32-byte master key, stored.
- **Joining**: a board broadcasts a HELLO (its address + type + an 8-byte HMAC tag only a holder of the secret
  can make) while it looks for HELM; HELM verifies it and answers with a HELLO_ACK broadcast carrying its own
  tag, so the board knows HELM is genuine too. Both then derive the key for that pair
  (`fsec_lmk(addrA, addrB)` = HMAC of the master key over the two radio addresses) and register each other as
  encrypted ESP-NOW peers. No key is ever transmitted; a stranger cannot join, nor impersonate HELM.
  HELM remembers joined boards (address + type) in NVS; keys are re-derived at boot.
- **Signed bus frames**: ESP-NOW cannot encrypt a broadcast, so every bus frame carries a random per-boot
  `session`, a `counter`, and an 8-byte HMAC `tag` over the sender's radio address and the frame. Receivers
  drop a bad tag and any counter that doesn't advance (a restarted sender may start over only below counter
  100). Limit: a receiver that has never heard a sender accepts the first valid frame, which could be an old
  recording. Addressed frames are also encrypted by the radio with the pair key.
- **Contact** is judged only from frames *addressed* to a board (`info->des_addr` not the broadcast address);
  HELM therefore also sends its heartbeat as an addressed unicast to every joined board.
- **Serial menu** on every board: `MENU`, `STATUS`, `KEY`, `KEYSHOW`, `KEYCLEAR`, `WIFI:<ssid>,<pass>`, `REBOOT`;
  HELM adds `UPDATE` (check, then Update All), `CLEARPEERS`, `CANON/CANOFF`; the CYD and can_sim add `REPAIR`.
- **Verified on the bench** (HELM + C3 can_sim + CYD): same phrase -> all join by themselves, same fingerprint,
  full data to the CYD, encrypted ignition commands HELM -> C3; a different phrase on the C3 -> HELM logs
  "HELLO rejected - not made with our key" and the C3 never joins; the right phrase back -> rejoins in ~1 s.
- **Serial-flashing a board that was ever updated over the air**: it boots from the OTHER program slot, so
  flashing slot 0 does nothing - also erase `otadata` (0xe000, 0x2000), as `tech_flash_hc.sh` does.

### Setup page / WEBMODE (`setup_web.h`, identical copies in `engine_display/` and `can_sim/`)

- A small web page to set the ESP-NOW passphrase and (optionally) WiFi details. On the HELM it is part of the
  normal web page (it is on your WiFi): `POST /key`, `/wifi`, `/reboot`, and a key form under "ESP-NOW".
- On a C3 or CYD, serial **`WEBMODE`** restarts the board into setup mode for 10 minutes (then it restarts
  normally): it joins the WiFi it has saved, if any, otherwise starts its own **open** network `EC-xxxx` (the
  last two address bytes; page at `http://192.168.4.1/`). The CYD also shows where to connect on its screen.
  The open network is a deliberate choice for convenience - anyone in range during those minutes could set a key;
  keep setup mode short and compare fingerprints afterwards (`KEYSHOW`).
- Saving a key forgets joined boards and restarts the board; saving WiFi details restarts it too. A CYD never joins
  WiFi in normal use (its role), so WiFi details only matter for `WEBMODE` on a C3/S3 `can_sim` or the HELM.
- Bench-verified: C3 and CYD setup pages reached from a laptop-style client, short passphrase refused, valid one
  saved, board rejoined the HELM by itself. Note the CYD's serial log goes quiet while it is in setup mode.

### Migrating from the pairing-window generation

The secured generation (HELM b36+ / can_sim b27+) cannot talk to the old one (different frame layout, and
ESP-NOW is off until a key is set). Update All still works to get the boards across, but afterwards **set the same
passphrase on every board** (serial `KEY ...` or the setup page) before anything joins; confirm the fingerprints match.

### The serial CLI (`serial_cli.h`, identical copies in `engine_display/` and `can_sim/`)

The serial console is a small line editor, so it can be used from a plain terminal (115200 baud; PuTTY, `screen`,
`minicom`, the Arduino monitor with "Both NL & CR"):

- **Tab** completes the command name; several matches extend to what they share, or list them.
  **`?`** lists the commands that match what is typed (all of them on an empty line) - only before the first
  space, so a passphrase may contain a `?`. `MENU` / `HELP` print the same list.
- Backspace; **Ctrl-U** clears the line; **Ctrl-C** abandons it; **Up / Down** recall the last six commands.
- After `KEY ` the characters echo as `*`, and a `KEY ...` line is never put in the history.
- Periodic status lines (`loop alive`, frame counters, engine commands received) are held back for 10 s after
  the last keypress (`CLI_LOG()` / `cli_quiet()`), so they don't land in the middle of what you are typing.
- One command table per sketch (`kCli[]`) feeds Tab, `?` and `MENU`; add a command there and in
  `handle_serial_line()`. Prompts: `HELM> `, `CYD> `, `SIM> `.
- **Trap**: in `engine_display.ino` the sketch replaces `Serial` with a logging wrapper (`#define Serial
  g_log_serial`). Any header that prints must be included AFTER that line, or its output goes to the core's
  original, never-begun Serial object and silently vanishes (this broke both the CLI echo and the CYD's
  setup-mode messages until the includes were moved).
