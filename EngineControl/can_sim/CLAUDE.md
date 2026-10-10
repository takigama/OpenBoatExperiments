# CAN bus node simulator (bench test rig)

Standalone Arduino sketch that pretends to be **up to 5 nodes** on the
MD-family CAN bus at once: up to 4 fully user-defined fake engine CTRL
boards + 1 "alarmer" unit (speaker/buzzer, reacts to alarm flags, honors
`CMD_ALARM_SILENCE`). Exists so the HELM/secondary displays in
`../engine_display` can be bench-tested against real CAN traffic from a
second physical node, instead of only the HELM's own local
(software-only) fake-engine debug page.

This is a sibling Arduino sketch to `../engine_display`, not part of it -
separate folder, separate `.ino`, compiles/flashes independently.

## Build / flash

- Arduino IDE (or arduino-cli), board **ESP32S3 Dev Module**
- Board: Waveshare **ESP32-S3-Zero** (ESP32-S3FH4R2 - 4MB flash, 2MB
  **quad** PSRAM - NOT the octal PSRAM the HELM board's N16R8 uses, so
  none of `engine_display`'s PSRAM/bounce-buffer constraints apply here)
- Flash size: 4MB. PSRAM: not used by this sketch - leave disabled.
- **Partition Scheme: "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)"**
  - explicitly pinned, not left on whatever the Arduino IDE happens to
  default to. This is the one 4MB scheme with a **2x1.25MB `ota_0`/
  `ota_1` pair**, which is what makes this board OTA-capable at all (see
  "OTA updates" below) - other 4MB schemes (e.g. "No OTA (2MB APP/2MB
  SPIFFS)") have only one app partition and silently break `Update.h`.
  Was previously left unpinned/implicit; confirm this is actually
  selected in Tools -> Partition Scheme before flashing, don't assume.
- **USB CDC On Boot: ENABLED** - this board uses the ESP32-S3's native
  USB (unlike the HELM board, which goes over a CH340 UART bridge and
  needs this DISABLED). Getting this backwards means no Serial output.
- Libraries: TWAI driver, WiFi, WebServer, Preferences, HTTPClient,
  HTTPUpdate, Update - all bundled with the ESP32 Arduino core, nothing
  extra to install. **ArduinoJson is NOT a dependency here** - unlike
  HELM, `can_sim` never parses the manifest itself, only receives an
  already-resolved URL from HELM over `MSG_OTA_START` (see below). No
  LVGL, no display panel library - there's no screen on this board at all.

## Files

- `can_sim.ino` — the whole sketch (framework stage - see below)
- `can_protocol.h` — **copy** of `../engine_display/can_protocol.h`. Arduino
  sketches can't share files across folders, so this is duplicated by
  hand. Keep both copies byte-identical; diff them if unsure which
  changed last. A comment at the top of each copy points at the other.
- `espnow_pairing.h` — **copy** of `../engine_display/espnow_pairing.h`,
  same duplication discipline. ESP-NOW pairing constants/message struct -
  see "ESP-NOW pairing" below.

## Pins (ESP32-S3-Zero)

Only GPIO 1-13 are used - the easily-solderable header pins on this
board. The rest are technically broken out but hard to solder to, so
they're left alone here even though nothing electrically prevents using
them.

- **CAN TX = GPIO 4, CAN RX = GPIO 5** (external 3.3V transceiver, e.g.
  SN65HVD230 - needs its own transceiver same as the HELM board would)
- **Buzzer/speaker = GPIO 6** (simple digitalWrite on/off toggle, not
  PWM - see the comment on `buzzer_tick()` for why, and how to upgrade
  to an actual tone if driving a passive speaker)
- **Direct-wire link TX = GPIO 1, RX = GPIO 2** (see "Direct-wire
  supplementary transport" below) - cross-wired to HELM's PIN_WIRED_RX/TX
  (GPIO 11/10), plus a common GND.
- Avoid: GPIO 0 (BOOT button/strap), GPIO 21 (onboard WS2812 RGB LED),
  GPIO 43/44 (default UART0 - left free for `Serial` debug output),
  GPIO 39-42 (JTAG), GPIO 19/20 (native USB D-/D+), GPIO 3/45/46
  (SoC-level strapping pins). GPIOs 7,8,15,16,17,18 are free for
  whatever comes next (status LED, a physical mute button, etc).

## What it pretends to be

Up to `MD_MAX_ENGINES` (4) fake engines, `sims[0..3]`, entirely defined
from the debug page rather than hardcoded - name, capability bits,
throttle behavior, and force-alarm test toggles (see "Debug page" below).
None has a hardcoded engine index - HELM (the bus master) assigns one
dynamically via the ENROLL_REQUEST/ENROLL_ASSIGN handshake in
`can_protocol.h`, keyed by MAC address, exactly like a real CTRL board
would get. One physical board only has one real MAC, so `derive_sim_mac()`
fakes distinct-but-stable MAC identities (4 engines + the alarmer) by
XORing the last byte of the real MAC with a small per-role tag - a real,
separate CTRL board would just use its own real MAC, no faking needed.
Each fake node broadcasts ENROLL_REQUEST every 2s until assigned (see the
Serial log / `/status` JSON's `enrolled`/`idx` fields) - if the display
side isn't running yet, or its enrollment table is full, they'll just
keep waiting/retrying.

All 4 are `ENGTYPE_GENERIC_DIESEL`, deliberately not user-selectable as
`ENGTYPE_MD2030`/`MD2030C` - `CAP_STOP` is a free checkbox on these
debug-defined engines, and the real MD2030 must NEVER be paired with
`CAP_STOP`, not even in simulation (see `../engine_display/CLAUDE.md`'s
safety model). The display-visible name comes from the free-text `name`
field instead, so the type enum isn't needed for that.

Default boot state: engines 0 and 1 enabled (matching the old fixed
SIM_A/SIM_B bench setup out of the box, engine 1 pre-checked for
`CAP_STOP` so the HELM electric-stop button has something to talk to);
engines 2/3 start disabled - turn them on from the debug page once
you've given them a name/capabilities.

Once enrolled, each engine also sends its display name (`MSG_ENGINE_NAME`,
chunked) every few seconds - whatever was typed into its debug-page Name
field (defaults to "SE1 - J1G1E2", "SE2 - J1G2E2", "SE3 - J4G8E4", "SE4 -
J8G12E12"), capped at `MD_ENGINE_NAME_MAXLEN` (24 chars) and sanitized to
plain ASCII.

Each simulated engine: sends ANNOUNCE (1Hz) / telemetry (100ms) / hours
(5s, only if its `CAP_HOURS` checkbox is on) / name (3s), honors
glow-held/start-held/ignition/stop commands **gated on the matching
capability checkbox** (a CTRL board with no glow relay wired up wouldn't
react to `CMD_GLOW_HELD` either, whether or not a display ever sends
it - same discipline `CAP_STOP` always had, now applied to all four),
and raises temp/press alarm flags past fixed thresholds
(`SIM_TEMP_ALARM_C`, `SIM_OIL_ALARM_BAR`) - also gated on `CAP_TEMP_ALARM`/
`CAP_PRESS_ALARM`.

**Start sequence** (glow/start held -> actually running): holding START
does nothing to the rpm reading for `SIM_CRANK_CATCH_MS` (5s) - the tach
doesn't register during cranking, same as a real diesel, though
`TFLAG_CRANK_ACTIVE` is set the whole time so the display's "cranking"
UI still lights up. At 5s it catches (a rough jump to ~40% idle), then
ramps up to `SIM_IDLE_RPM` over the next second or so. Coolant temp
climbs toward `SIM_OPERATING_TEMP_C` and oil pressure builds toward its
rpm-derived target, both as an exponential approach (gradual, not an
instant jump) once running. Releasing START before 5s is up aborts the
start entirely - nothing happens, matching "let go of the key before it
fires and it doesn't start."

**Stop** (`CAP_STOP` checked only): cuts `running` immediately but does
NOT zero the rpm - it decelerates naturally via `SIM_SPINDOWN_PER_S`,
same coastdown behavior as fuel starvation or any other engine-off case,
rather than an unrealistic instant stop.

**Throttle behavior** (debug-page dropdown, per engine): **Idle** sits
steady at `SIM_IDLE_RPM` while running; **Random rev** wanders between
`SIM_RANDOM_REV_MIN` (1500) and `SIM_RANDOM_REV_MAX` (3700 - just over
the display's shared 3600 redline) at `SIM_RANDOM_REV_RAMP_PER_S`,
picking a new random target every 4-12s. Random rev deliberately ramps
rather than jumping, so it reads as a lazy hand on the throttle, not
noise - and it lets you drive the display's red-zone/overrev alarm logic
on demand without sitting there manually dragging a slider for 5+ seconds.

**Force alarm** (debug-page checkboxes, per engine, one each for temp/
press/charge/water): OR'd with the natural physics-derived condition, and
still gated on the matching capability checkbox (no sensor checked means
nothing to have tripped, so the force toggle can't matter either). Charge
and water have **no physics model at all** - there's no simulated
alternator or fuel-water sensor - so the force checkbox is the *only* way
to ever raise `CAP_CHARGE_ALARM`/`CAP_WATER_ALARM`'s flags on the bus.
That's intentional: it exists purely to bench-test the alarmer/display's
reaction to those two capabilities. These are separate from - and
persist across - the Enabled checkbox; disabling/re-enabling an engine
resets its simulated physical state (rpm/temp/oil) but not its
name/capabilities/throttle mode/force-alarm toggles, which are user
configuration, not power state.

**Force fail to start** (debug-page checkbox, per engine, "Starter test"
row): when set, `sim_engine_physics_tick()`'s crank-catch transition
(`SIM_CRANK_CATCH_MS`) never fires - the engine cranks (with the usual
RPM-jitter twitter) forever without catching, until `start_held` is
released, either by the operator letting go or by the dead-man's own
`CRANK_MAX_MS` cutoff (`sim_engine_deadman_tick()`), exactly mirroring a
real stuck-crank scenario. Exists to bench-test
`../engine_display`'s Autostart retry logic (glow -> crank -> wait ->
re-crank) against an engine that genuinely never starts. Same discipline
as the force-alarm toggles above: not touched by `sim_engine_reset()`,
persists across the Enabled checkbox.

The alarmer role is **not** hard-wired to any particular engine slot - it
listens to every engine index's telemetry it can hear on the bus (so
it'll react to a real fifth engine too, not just this board's own four),
tracks per-engine alarm-active/silenced state (all four alarm types, not
just temp/press) the same way the display's `silenced_mask` does, and
beeps GPIO 6 whenever anything's unsilenced. It also enrolls with its own
synthetic MAC (`NODE_TYPE_ALARMER`) purely so HELM's enrollment table has
a real record of it - its assigned id doesn't gate any of its behavior
today, since it already listens to the whole bus regardless of what id
it got.

## WiFi debug page + serial console

Joins WiFi the same *shape* as the HELM panel: blocking join, NVS
persistence ("cansim" namespace). With no credentials configured (or a
failed join), this board deliberately does NOT fall back to
broadcasting its own "CAN-SIM" AP - that's unrequested WiFi activity on
a board that's often run with zero WiFi at all. It stays in `WIFI_STA`
mode with no active connection, starting on a fixed channel
(`ESPNOW_AP_FALLBACK_CHANNEL`, `espnow_pairing.h`) via
`esp_wifi_set_channel()` so ESP-NOW still works - the debug page is
simply unreachable until `WIFI:<ssid>,<password>` succeeds. That starting
channel is only ever a first guess: `pairing_requester_tick()` actively
hunts from there for HELM's real channel (see "ESP-NOW pairing +
transport" below) rather than assuming it's correct, since HELM - the
only node expected to ever join real WiFi - could be on any channel its
router assigned.
`WIFI_SSID`/`WIFI_PASSWORD` `#define`s near the top of `can_sim.ino` are
the same "non-empty compiled-in wins and gets saved, blank falls back to
NVS" resolution rule the panel uses.

This board has no screen, so there's no setup wizard or PIN lock - but
it still needs a way to get WiFi creds onto it day-to-day without
reflashing. That's what the serial console is for (not just a bench
convenience like it is on the panel - here it's the primary way to
configure WiFi): send `WIFI:<ssid>,<password>` over USB serial at any
time and it saves to NVS + reboots, identical to the panel's feature
(see `handle_serial_line`/`serial_console_tick`).

The debug page (served at `/`) has one card per engine (0-3), each with:

- **Enabled** checkbox - vanishes off the bus entirely when off, like
  the real CTRL board being powered down; also resets the engine to a
  clean physical state so re-enabling doesn't resume mid-crank.
- **Name** free-text field (up to 24 chars, sanitized to plain ASCII on
  save) - sent over `MSG_ENGINE_NAME`, this is what the HELM/CYD display
  shows instead of a generic type name.
- **Throttle** dropdown: Idle / Random rev (see above).
- 13 **capability** checkboxes, one per `CAP_*` bit (RPM, coolant temp,
  oil pressure, temp alarm, press alarm, glow, start, ignition, hours,
  audio, electric stop, charge alarm, water-in-fuel alarm) - these
  aren't just cosmetic, they gate what the sim actually does (see "What
  it pretends to be" above).
- 4 **force alarm** checkboxes (temp/press/charge/water) for on-demand
  alarm testing.
- A live state/rpm/temp/oil/hours/enrollment readout polled from
  `/status` (JSON) once a second.

All engine fields for a given card are sent together in one `/set`
request (`i=<engine>&en=&nm=&md=&c_rpm=...&f_temp=...`) whenever any
control in that card changes - see `handle_set()`/`CAP_CHECKBOXES` in
`can_sim.ino` (the checkbox-id-to-`CAP_*`-bit table both the HTML
template and the request handler iterate over, so they can't drift out
of sync with each other). Unlike the HELM panel's debug page, there's no
manual rpm/temp/oil override sliders here - the whole point of this
board is that the physics run autonomously; the page only defines what
each engine IS and toggles which behaviors are active.

## ESP-NOW pairing + transport

This board runs WiFi (as below) and ESP-NOW simultaneously, and is always
the pairing **requester** (HELM is the acceptor - see
`../engine_display/CLAUDE.md`). Once paired, real protocol traffic - not
just the handshake - flows over ESP-NOW whenever **Disable CAN** is on
(see below): enrollment, ANNOUNCE/telemetry/hours/name, and receiving the
4 dead-man/ignition commands, all the same `can_protocol.h` messages CAN
carries, tunneled through `espnow_bus.h`'s small envelope. This board
never *sends* the 4 unicast commands (only HELM does, to whichever
transport a given engine is actually on) - it only ever broadcasts, since
it has no reason to unicast anything.

- **Channel-hunting, not a fixed channel**: HELM is the only node ever
  expected to join real WiFi (its router-assigned channel isn't knowable
  in advance), so this board doesn't assume a fixed channel - whenever it
  hasn't joined real WiFi itself (`g_wifi_joined` false - covers both "no
  credentials" and "credentials given but the join failed", since both
  land on the same fixed fallback channel) and either isn't paired yet or
  hasn't heard from its already-paired HELM in `LOST_CONTACT_MS` (HELM's
  channel changed out from under it, e.g. it rebooted onto a different
  network), `pairing_requester_tick()` sweeps candidate channels 1..13,
  dwelling `CHANNEL_DWELL_MS` (2s) on each - long enough to catch HELM's
  1Hz heartbeat if it's there - broadcasting `PAIR_MSG_REQUEST` on each
  candidate while unpaired. Once paired, no separate "still there?" probe
  is needed - the regular ANNOUNCE/TELEM broadcast traffic itself serves
  as the probe once the right channel is found. The last channel that
  actually worked is cached in NVS (`lastch`) and tried first on the next
  hunt - **`CACHED_CHANNEL_RETRIES` (5) extra dwells on it** before
  falling back to the full 1..13 sweep, not just one. Added after real-
  hardware testing showed a single missed/collided packet on the cached
  channel was sending this board straight into a slow full sweep even
  though HELM was right there on the same channel it always is - a
  reboot (including right after this board updates itself via OTA) is
  very likely to land back on the exact channel HELM was already using,
  so it's worth ~10s of retries on that one good guess before assuming
  it's actually gone.
- Once it receives a `PAIR_MSG_ACK` (persisted NVS `paired`) and contact
  is confirmed, it stops hunting/broadcasting entirely, including across
  future reboots (subject to the lost-contact re-hunt above) - it never
  re-enters pairing mode on its own otherwise.
- **`REPAIR`** over serial (same mechanism as `WIFI:<ssid>,<password>`,
  see below) clears the paired flag, which `pairing_requester_tick()`
  picks up on its very next call and starts hunting again with no other
  trigger needed. The debug page's **Clear Pairing** button does the same
  thing over HTTP. Both go through `espnow_forget_helm()`, which also
  drops HELM as an ESP-NOW peer and wipes its saved MAC/key, not just the
  `paired` flag.
- The debug page's pairing status row also shows the board's **live
  radio channel** (`/status`'s `"channel"` field, `WiFi.channel()` -
  deliberately not `g_channel`, the hunt/candidate variable, since
  `WiFi.channel()` reflects reality even when `g_wifi_joined` is true and
  a real router picked the channel, a case `pairing_requester_tick()`
  returns early on and never touches `g_channel` for). Added specifically
  to help diagnose a pairing failure without needing this board's own
  Serial output too - watch this alongside HELM's Serial Log debug-page
  panel (`../engine_display/CLAUDE.md`) to see whether the two boards
  ever land on the same channel during a hunt.
- **Encrypted, not just allowlisted**: the `PAIR_MSG_ACK` carries a
  16-byte key HELM generated for this specific pairing. This board saves
  HELM's MAC + that key (NVS `helm_mac`/`helm_lmk`) and registers HELM as
  an `encrypt=true` ESP-NOW peer - all traffic between the two is
  transparently AES-encrypted from then on. On a later boot, if already
  paired, `espnow_setup()` re-registers HELM as an encrypted peer from
  the saved MAC/key without re-broadcasting; if the `paired` flag is set
  but no key was ever saved (e.g. this board was paired under the older
  plaintext infrastructure-only version), it auto-detects that and
  re-arms pairing rather than getting stuck. See
  `../engine_display/CLAUDE.md`'s "AES pairing" section for the full
  design (key generation, the necessarily-plaintext ACK, `ESPNOW_PMK`).
- Debug page also has a **CAN Enabled** checkbox (persisted NVS
  `can_dis`, inverted sense - unchecked means `g_can_disabled=true`,
  default checked/enabled) - unchecking it skips `can_setup()`'s TWAI
  init entirely at boot. `bus_send()` (replaces the old CAN-only
  `can_send()`, same signature) routes on `!g_can_disabled && can_ok` -
  BOTH the user's preference AND actual CAN health, not the preference
  alone. This matters because `can_bus_health_tick()` can flip `can_ok`
  to `false` on its own (repeated bus-off with no transceiver wired)
  without ever touching `g_can_disabled` - a board that boots with CAN
  enabled but no transceiver attached would otherwise silently blackhole
  every message once bus-off auto-disabled CAN, rather than falling back
  to an already-paired ESP-NOW peer. Falls back to ESP-NOW broadcast
  whenever CAN isn't actually usable (disabled OR unhealthy) and paired;
  silent no-op if neither is available - no separate per-message
  transport tracking needed, unlike HELM, since this is one physical
  board with one uplink. `can_poll()`'s old inline dispatch was extracted
  into `bus_handle_rx()` so both the real CAN loop and the ESP-NOW
  receive path feed the exact same handler.
- `FLEET_ID` and `ESPNOW_PMK` (`espnow_pairing.h`) must match
  `../engine_display`'s copy exactly, and both should be unique to your
  boat - see that file's comments. `espnow_bus.h` (the transport envelope)
  is duplicated the same way.

## Direct-wire supplementary transport (`wired_bus.h`)

A third transport, alongside CAN and ESP-NOW, for when this board and
HELM are physically right next to each other - a bench setup, or (for a
future real CTRL board) mounted right behind the panel - and running 3
wires is simpler than a CAN transceiver or relying on radio. See
`../engine_display/CLAUDE.md`'s matching section for the full design
(`wired_bus.h`'s own header comment is the authoritative writeup, shared
byte-identical between both copies); this covers only this board's side.

- **UART1** (`WiredSerial`) on GPIO 1 (TX) / GPIO 2 (RX) - cross-wired to
  HELM's GPIO 11 (RX) / GPIO 10 (TX), plus a common GND. Completely
  separate from UART0 (this board's native-USB `Serial` console).
- **Always-on**: `wired_setup()` runs unconditionally in `setup()`,
  matching `can_setup()`'s own "always try, note whether it worked"
  shape - a disconnected RX pin reading noise just fails the frame
  checksum and gets dropped, same tolerance already accepted for CAN
  with no transceiver wired.
- **`bus_send()` always attempts it** (`if (g_wired_ok) wired_send_frame
  (...)`), independent of - not instead of - the existing CAN-vs-ESP-NOW
  choice right below it: this board's `bus_send()` treats CAN and
  ESP-NOW as mutually exclusive alternatives (only one is ever the
  "real" uplink at a time), but wired is genuinely supplementary and
  fires on every call regardless of which of those two wins.
- **`wired_bus_tick()`** (called from `loop()`) parses incoming frames
  with the same fixed-length-after-sync/checksum discipline as HELM's
  copy, and hands a good frame straight to `bus_handle_rx()` - the same
  function the real CAN loop and the ESP-NOW bus-frame path already
  share, so this board needed zero new command-handling logic, just a
  third way for a `twai_message_t` to arrive. Unlike HELM's side, there's
  no per-engine transport stamping here - this board is the slave/one-
  uplink side (see `bus_send()`'s own comment above), so it doesn't need
  to remember which transport a command came in on.
- **Debug page shows "Wired link: receiving/no signal"** (`g_wired_last_
  rx_ms`, `/status`'s `"wired_active"` field - a good frame within the
  last 3s) - added for the same reason the live WiFi channel is shown:
  no other way to confirm the 3-wire hookup is actually working without
  a serial cable already attached to watch the log.

## OTA updates (remote-triggered by HELM)

This board has no display/manifest-checking logic of its own - it never
fetches `OTA_MANIFEST_URL` or compares build numbers itself. It only
reports its own `FW_BUILD` (packed into `ANNOUNCE`'s `[5..6]` bytes,
`sim_engine_send_announce()`) and its hardware (`HW_ID`, byte `[7]` -
HELM uses it to pick this board's image out of the manifest; a new chip
or board needs its own `HW_*` id, see `can_protocol.h`, and a variant in
`../tools/devices.json`) and reacts when HELM decides an update is
due and pushes one down. See `../engine_display/CLAUDE.md`'s "OTA
updates" section (Phase 2) for HELM's side of this - this section covers
only what happens here.

- **`ota_handle_start_chunk()`** reassembles `MSG_OTA_START(e)` (chunked,
  same `total_len`/`chunk_idx` framing as `MSG_ENGINE_NAME`, see
  `can_protocol.h`'s comment) into `ssid\0password\0url\0md5\0`, but only
  for an engine index that's actually one of this board's own enrolled
  engines (`sims[i].enrolled && sims[i].idx == e`) - a stray frame
  addressed to some other board's engine index is ignored **and now
  logged** (`"...ignored - not one of this board's enrolled engines"`) -
  previously silent, which made a real-hardware failure (HELM's idea of
  an engine's index no longer matching what this board actually has
  enrolled, e.g. after a re-enrollment onto a different slot)
  indistinguishable from ordinary packet loss. `md5` may be an empty
  string (just its own NUL) if HELM's own manifest fetch had none for
  this entry - not a malformed-payload error, just "nothing to verify
  against" (see `ota_tick()` below).
  `chunk_idx == 0` (re)starts a fresh reassembly (now logged too, with
  the expected total byte count), so a retried burst (e.g. HELM never
  got the ACK) can't get stuck merged with a stale partial reassembly
  from an earlier attempt. Every individual chunk's arrival is logged
  (`"chunk N/M received for engine E"`) - since reassembly requires
  *every* expected chunk and there's no partial-timeout recovery (a
  fresh `chunk_idx==0` burst is the only way to un-stick it), a gap in
  this sequence in the Serial log directly shows which chunk(s) got lost
  on the way here, rather than just "it never ACKed" with no further
  clue. Does **not** touch WiFi from here - this can run off an ESP-NOW
  receive callback, and this board's own
  `../engine_display/CLAUDE.md`-documented rule ("WiFi APIs may only be
  touched from the one proven place") extends to any ESP-NOW receive
  context too, not just the LVGL task HELM worries about - it just sets
  `g_ota_ready`/`g_ota_engine` and returns.
- **`ota_tick()`** (called from `loop()`, the one safe WiFi-touching
  context) does the actual work once `g_ota_ready` is set: sends
  `MSG_OTA_ACK(g_ota_engine)` **3 times, 50ms apart, then an extra 50ms
  settle delay** before touching WiFi (was a single send + 100ms delay).
  Unlike `ENROLL_REQUEST` (repeated broadcast until acknowledged) or the
  dead-man commands (`HOLD_RESEND_MS` resend loop), this ACK has no
  retry mechanism of its own once `WiFi.begin()` disrupts the ESP-NOW
  channel - a single lost packet here used to mean HELM could never
  learn a genuinely successful update happened at all. Confirmed on real
  hardware: this board completed a full update (visible via its own
  fresh `ENROLL_REQUEST` after reboot) while HELM's UI still timed out
  waiting for an ACK that evidently never arrived - sending 3 copies
  reduces but doesn't eliminate that risk, which is why HELM's
  `remote_ota_modal_tick()` (see `../engine_display/CLAUDE.md`) also
  independently confirms success via this board's next `ANNOUNCE`
  `fw_build` catching up to the expected build, not just the ACK alone.
  Then `WiFi.mode(WIFI_STA); WiFi.begin(ssid, pass)`
  with a 20s connect timeout, then `httpUpdate.setMD5sum(g_ota_md5)` if
  `g_ota_md5` isn't empty (HTTPUpdate's own built-in checksum mechanism -
  `Update.end()` rejects a mismatched image outright, same as HELM's own
  self-update, see that file's CLAUDE.md), then on success
  `httpUpdate.rebootOnUpdate(true); httpUpdate.update(client, url)`.
  **Unconditionally calls `ESP.restart()` at the end regardless of
  outcome** - a successful flash already rebooted via
  `rebootOnUpdate(true)`, and any failure path (bad creds, unreachable
  URL, bad manifest) restarts too, so the board cleanly resumes normal
  ESP-NOW/CAN operation rather than being stuck mid-WiFi-join forever.
- **Rollback safety**: same `esp_ota_mark_app_valid_cancel_rollback()`
  call in `setup()` as HELM's, same caveat that this is a no-op if the
  installed core doesn't have rollback compiled in - see
  `../engine_display/CLAUDE.md`'s Phase 1 section for the full reasoning.
  Requires the explicit `PartitionScheme=default` pin above (`ota_0`/
  `ota_1` pair) - without it `Update.begin()` fails outright.
- **No device-type field yet**: HELM currently assumes every remote
  `fw_build` it sees belongs to `can_sim`'s own manifest entry (there's
  only one kind of remote OTA target right now). A future real CTRL
  board will need its own way to say which manifest entry it is -
  tracked as a known gap in `../engine_display/CLAUDE.md`, not built
  here yet.

## Remote WiFi join (debug/admin trigger)

HELM's debug page can hand this board its own saved WiFi credentials
over the bus (`MSG_WIFI_JOIN(e)`) and tell it to join and persist that
join - see `../engine_display/CLAUDE.md`'s "Remote WiFi join" section for
HELM's sending side. This section covers only what happens here.

- **`wifi_join_handle_start_chunk()`** reassembles the chunked burst
  (same `total_len`/`chunk_idx` framing as `MSG_OTA_START`, minus the
  url/md5 fields - just `"ssid\0password\0"`) - same enrolled-engine
  ownership check and `chunk_idx==0`-restarts-reassembly discipline as
  `ota_handle_start_chunk()`. Does **not** touch WiFi/NVS here - like the
  OTA path, this can run off an ESP-NOW receive callback, so it just sets
  `g_wifi_join_ready` and returns; `wifi_join_tick()` (called from
  `loop()`, the one safe WiFi/NVS-then-reboot context) does the actual
  work.
- **`save_wifi_and_reboot(ssid, pass)`** is the single function that
  actually saves credentials and reboots - factored out so both the
  serial `WIFI:<ssid>,<password>` command and `wifi_join_tick()` share
  the exact same save-then-restart logic rather than two copies drifting
  apart. Empty SSID or unavailable NVS are handled the same way
  regardless of which path triggered it (logged and ignored, no reboot).
  This means the bus-triggered join has the identical persistence
  behavior as typing the command over USB: saved to NVS, survives future
  reboots, not a one-off `WiFi.begin()` that would be lost on the next
  power cycle.
- **No ACK/confirmation message for this one** (unlike `MSG_OTA_START`'s
  `MSG_OTA_ACK`) - it's a debug/admin convenience, not a safety-relevant
  flow, and the board rebooting to join is itself the visible proof of
  receipt (it briefly drops off the bus, then reappears once connected).

## Status: framework, not finished

This is a working skeleton, not a finished simulator:

- Physics (rpm ramp, temp/oil curves, crank timing) are simple
  placeholders tuned by eye, not measured against any real engine's
  actual behavior. All 4 engine slots share identical constants - no
  per-engine spec data, despite the name/capabilities now being
  user-definable per slot.
- Bus enrollment (MAC-based dynamic addressing, see "What it pretends to
  be" above) is new and hasn't been bench-tested against a real HELM
  board yet - reviewed by hand only, same caveat as everything else here.
  If an engine never leaves "not enrolled" in the Serial log, check that
  HELM is actually running/on the bus (it's the only thing that answers
  ENROLL_REQUEST) before suspecting the sim side.
- No bus-off/no-transceiver hardening beyond a basic recovery/disable
  loop (see `can_bus_health_tick`) - untested against a real bad-wiring
  scenario yet.
- Buzzer is a bare digitalWrite toggle. Fine for an active buzzer
  module; upgrade to LEDC PWM for a passive speaker with an actual tone
  (left as plain digitalWrite specifically to dodge the `ledcSetup`
  vs `ledcAttach` API split between arduino-esp32 2.x and 3.x - pick
  whichever matches your installed core version when you get there).
- Not yet compiled/flashed against real hardware - written and
  reviewed by hand (brace/paren balance, symbol-by-symbol wiring
  check), same caveat as everything in `../engine_display` built this way.
- Custom typedefs in a function's parameter list break Arduino's
  auto-prototype generation (it hoists prototypes above ALL user code,
  including typedefs defined earlier in the same file) - hit this once
  already in `engine_display.ino`. Avoid the pattern here too if this file
  grows more callback-style helpers.
