# Man Overboard via Find Hub tags — investigation notes

Everything discovered while investigating whether off-the-shelf Google Find Hub
Network tags (MiLi MiTag) could be resolved locally by our own hardware
(ESP32-C3 sensor boards / CYD hub), for a man-overboard detection system on
OpenBoat. Written up so this doesn't need to be re-derived from scratch in a
future session.

**Goal**: attach a cheap, waterproof, sealed Find Hub tag to each crew
member's life vest. The boat's own hardware (3x ESP32-C3 sensor boards + a
CYD hub) listens for the tag's BLE broadcasts and can tell if someone's tag
has gone out of range - all fully offline, no Google account/cloud
involvement at runtime.

---

## 1. Why this is possible at all (crypto background)

Google's Find Hub Network (FMDN, an extension of the Fast Pair spec) makes a
tag broadcast a **rotating 20-byte identifier (EID)** every ~1024 seconds,
computed from a 32-byte secret called the **EIK (Ephemeral Identity Key)**.
Anyone who knows a tag's EIK can independently compute what it's currently
broadcasting and recognize it - entirely offline, no need to talk to Google
at runtime. The hard part is *getting* the EIK in the first place.

### 1.1 Two different keys - don't confuse them
- **Anti-spoofing key** (per Fast Pair *model*, shared across every unit of
  that product): used only for the *initial* "is this a genuine accessory"
  pairing handshake. Nothing to do with location privacy.
- **EIK** (unique *per tracker*, generated fresh by the phone at provisioning
  time): this is the one that actually drives the rotating identifier. Never
  transmitted to Google in raw form - only two SHA-256-derived sub-keys
  (Recovery key, Ring key) are, so Google can offer "locate"/"ring" features
  without ever holding the real key.

Trying to build our own "Seeker" from scratch (to inject our own EIK into a
tag) hits a real wall: the initial pairing handshake requires knowing that
specific product model's anti-spoofing *public* key, which Google doesn't
publish openly - you'd need to intercept a real phone's traffic to get it.
**We didn't need to go down this path** - see below.

### 1.2 The actual working method: extract the EIK Google already has
Google backs up each tracker's EIK to your account's cloud key vault
(E2E-encrypted with your device's screen-lock PIN) as a side effect of normal
Find Hub usage. An open-source tool already automates pulling it back out:

**[GoogleFindMyTools](https://github.com/leonboe1/GoogleFindMyTools)** (Python,
kept at `D:\temp\claude\GoogleFindMyTools` - **not** moved into this repo,
since its `Auth/secrets.json` holds real OAuth tokens and should never be
committed anywhere).

Setup (already done once, should still work):
```bash
cd /d/temp/claude/GoogleFindMyTools
source venv/Scripts/activate
python main.py
```
- First run opens a real Chrome window for you to sign into your own Google
  account (Claude never touches this step - it's your credentials).
- Lists all registered trackers; pick one by number to fetch its location AND
  print its EIK (we patched `NovaApi/ExecuteAction/LocateTracker/decrypt_locations.py`
  to add `print(f"[EIK] Identity Key (hex): {identity_key.hex()}")` right
  after it's derived - not upstream behavior).
- A second Chrome sign-in prompt happens the first time it needs the E2EE
  vault key specifically (`KeyBackup/shared_key_flow.py` - opens
  accounts.google.com, waits for you to sign in, then rides that authenticated
  session to ask Google's own internal "security domain" page for the vault
  key via an injected JS callback). Nothing adversarial - it's just using your
  own legitimate session the same way a second device restoring a backup
  would.
- Can also trigger "play sound" on a specific registered tag (see
  `NovaApi/ExecuteAction/PlaySound/start_sound_request.py`) - useful for
  confirming which physical unit corresponds to which account entry. Note:
  this goes through Google's cloud (HTTPS to their "Nova" API), which then
  relays the ring command to whichever nearby phone is part of the Find Hub
  network - **not** a direct Bluetooth action from this machine.

---

## 2. Real over-the-air broadcast format

Confirmed against the accessory-side reference firmware in the same repo
(`GoogleFindMyTools/ESP32Firmware/main/main.c`, which builds this exact frame
from the tag's side):

```
BLE advertisement, Service Data (AD type 0x16), 16-bit UUID 0xFEAA:
  byte 0:      frame type (0x40 or 0x41 - the latter signals "unwanted
               tracking protection" mode active; doesn't change the EID math)
  bytes 1-20:  the 20-byte rotating EID
  byte 21:     hashed flags (unused by us)
```

Confirmed via the actual `arduino-esp32` BLE library source
(`BLEAdvertisedDevice.cpp`) that `getServiceData()` already strips the 2-byte
UUID before returning - so byte offsets above are exactly what you get from
`advertisedDevice.getServiceData()`.

---

## 3. EID generation algorithm

Ported from `GoogleFindMyTools/FMDNCrypto/eid_generator.py` to C/mbedTLS. Full
implementation in `src/full_featured.cpp` (`generate_eid()`). Summary:

1. Mask the current Unix timestamp to a 1024-second window (`K=10`,
   `ROTATION_PERIOD = 2^K = 1024`): zero the low 10 bits.
2. Build a 32-byte buffer: `0xFF * 11 || K || masked_timestamp(4 bytes) ||
   0x00 * 11 || K || masked_timestamp(4 bytes)`.
3. AES-256-ECB encrypt that buffer with the EIK as the key -> `r'` (32 bytes).
   **Gotcha**: `mbedtls_aes_crypt_ecb()` only encrypts a single 16-byte block
   per call - it does NOT loop over multi-block buffers the way Python's
   PyCryptodome does. Need two separate calls for this 32-byte (2-block)
   structure, or the second half silently comes out as garbage.
4. Reduce `r'` (as a big-endian 256-bit integer) mod `n` (curve order) -> `r`.
5. Compute `R = r * G` on curve **SECP160r1** (not P-256 - a 160-bit curve).
6. The EID is the X-coordinate of `R`, as 20 bytes big-endian.

### mbedTLS gotchas on the ESP32 (arduino-esp32 / pioarduino)
- **SECP160R1 isn't compiled in** - this build's `mbedtls_ecp_group_id` enum
  only starts at `MBEDTLS_ECP_DP_SECP192R1`. Had to manually populate an
  `mbedtls_ecp_group` with the raw SEC2 domain parameters instead of using
  `mbedtls_ecp_group_load()`. Parameters (pulled from Python's `ecdsa`
  library directly, not transcribed by hand, to rule out a digit-transposition
  bug):
  ```
  P  = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFF
  A  = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFC
  B  = 1C97BEFC54BD7A8B65ACF89F81D4D4ADC565FA45
  Gx = 4A96B5688EF573284664698968C38BB913CBFC82
  Gy = 23A628553168947D59DCC912042351377AC5FB32
  N  = 0100000000000000000001F4C8F927AED3CA752257
  ```
- `mbedtls_ecp_point`'s `X`/`Y`/`Z` fields are wrapped in `MBEDTLS_PRIVATE()` in
  this mbedTLS version - need `#define MBEDTLS_ALLOW_PRIVATE_ACCESS` **before**
  including any mbedtls headers to get plain field access back.
- `mbedtls_ecp_mul()` in this version **requires a non-NULL `f_rng`** (for
  blinding countermeasures) - passing `NULL` returns
  `MBEDTLS_ERR_ECP_BAD_INPUT_DATA` immediately (confirmed by reading
  `ecp.c`'s `mbedtls_ecp_mul_restartable()` directly). Wired up a real
  `mbedtls_ctr_drbg`/`mbedtls_entropy` context, seeded once in `crypto_init()`
  and reused - re-seeding per call would be wasteful.
- Doing the actual EC math (~2.1 seconds/call, see benchmark below) **inside
  a BLE `onResult()` callback will crash the watchdog** - that callback runs
  in the Bluetooth stack's own task (`BTC_TASK`) and blocking it too long
  starves the idle task. Fixed by having `onResult()` only do the cheap
  frame-format check and hand the raw 20 bytes off via a small mutex-guarded
  queue (`portMUX_TYPE`) to `loop()`, which does the actual crypto comparison
  outside the BLE stack's task.

---

## 4. Performance benchmark (real hardware, classic ESP32 / denky32 @ 240MHz)

```
200 calls in 423,745,403 us  ->  2,118,727 us/call (~2.12 sec/call, 0.5 calls/sec)
```

Extrapolated full brute-force search, "tag could have been provisioned any
time since 2018" (262,925 windows @ 1024s): **~557,066 sec (~154.7 hours,
~6.4 days)**. Far too slow for a real-time search from scratch.

**Not yet done, but the obvious next optimization**: write a fast custom
`modp` reduction function exploiting SECP160r1's pseudo-Mersenne-shaped prime
(same trick mbedTLS's built-in NIST-curve fast paths use) instead of falling
back to generic bignum division on every operation - our manually-loaded
group has no `modp` hook at all right now. Likely a large (order-of-magnitude)
win. The RNG-blinding requirement is also pure overhead for this use case
(we're recognizing a known device, not protecting our own secret key from a
timing attacker) but the current public mbedTLS API doesn't offer a way
around it.

**However** - see section 5, this search doesn't need to span "since 2018" at
all in practice.

---

## 5. Device-clock behavior (the actual big discovery)

The tag's internal EID-rotation timestamp is **not synced to real UTC** at
all - confirmed by brute-forcing a captured live EID against increasingly
wide time ranges until a match was found, since a naive ±1 window search
around real "now" (to allow for clock skew) found nothing even up to ±7 days.

### 5.1 Known tags and their discovered "base" device-times
All three physical tags tested, regardless of which order they were bought in
(2 different orders, 2 months apart), landed their **fixed reset reference**
within a 23-minute window on the exact same calendar day:

| Tag name | Canonic ID | EIK (hex) | Base device-time (epoch) | UTC date/time |
|---|---|---|---|---|
| Kyuubi | `6a587aec-0000-2e2a-8423-ac3eb14f5988` | *(redacted - see `src/secrets.h`)* | `1741145927` | 2025-03-05 03:38:47 |
| Nothing Much | `6a58a4ca-0000-2213-b3b3-d43a2cc4d3ff` | *(redacted - see `src/secrets.h`)* | `1741144555` | 2025-03-05 03:15:55 |
| Nothing Much 2 | `6a59ace2-0000-2d24-8ddf-3c286d455d1e` | *(redacted - see `src/secrets.h`)* | `1741144818` | 2025-03-05 03:20:18 |

(Kyuubi purchased ~August 2025; Nothing Much + Nothing Much 2 both from the
same ~October 2025 order - the two same-batch units land only 4m23s apart
from each other, while Kyuubi from a separate order sits ~18-23 minutes
further down the sequence. Consistent with units having their clocks
initialized sequentially on a manufacturing/test line.)

**These are real secrets** - anyone with one of these EIK values can compute
that specific tag's location-identifying broadcasts. That's why they live only
in `src/secrets.h` (gitignored, see `src/no_secrets.h` for the template)
and not in this doc or anywhere else that's git-tracked.

### 5.2 What resetting (battery pull) actually does
Tested by pulling each tag's battery, waiting 30-60s, reinserting:
- A reset does **not** simply pause-and-resume the counter (that would only
  lose however many seconds it was actually unpowered). Kyuubi's first reset
  jumped its device-time **backward by ~8.5 hours** despite only a real-world
  gap of ~19 minutes between readings.
- A reset **reliably returns to the exact same fixed value every time** -
  confirmed byte-for-byte identical EIDs across repeated resets (Kyuubi's 2nd
  reset matched its 1st reset exactly; Nothing Much's reset matched its
  very-first-ever reading exactly).
- Nothing Much's battery had been physically installed since ~October 2025
  but it wasn't paired/activated until the day of this test - yet its very
  first reading still showed the ~March 2025 reference, not something
  reflecting ~9 months of continuous real-time ticking. This rules out "the
  counter just runs continuously from whenever the battery went in."

### 5.3 Working model
- Each tag has a **fixed factory/manufacturing-calibration timestamp** baked
  in (shared closely across units from the same/nearby production batches).
- The counter is **dormant while unpaired/in shipping mode** (even with the
  battery installed) - doesn't advance until first activated. Makes sense as
  a battery-conservation measure for units sitting in a warehouse/on a shelf.
- Once activated, it ticks normally in real elapsed seconds.
- Any reset (battery pull, and presumably any other power interruption -
  vibration, cold, corrosion at the contacts) reverts it back to that same
  fixed factory baseline, and it starts ticking again from there.

### 5.4 Practical implication for the real system
This turns the offline "guess the offset" problem from "continuous risk of a
multi-day brute force at sea" into something much more manageable:
- **Discovering a given physical tag's offset is a true one-time cost.** Once
  found, cache it (NVS) permanently for that tag.
- **Future resets are cheap to recover from** - just re-check a narrow window
  around the already-cached reference, not a full re-search.
- **First-time discovery for a brand new, never-before-seen tag from this
  same product line** can likely start with a narrow search around
  `2025-03-05 UTC ± a day or so` before ever falling back to a wider range -
  worth confirming with a 4th/different-batch tag if the opportunity comes up,
  since we've only ever tested tags that all happen to be MiLi MiTags.

---

## 6. MOB detection design considerations (not yet implemented)

- Tags broadcast their advertisement packet roughly every ~5 seconds
  (observed empirically - separate from the 1024s EID *rotation* period,
  which is how often the *value* changes, not how often it's *sent*).
- Missed-broadcast threshold: reasoned framework, not yet validated at sea:
  - Don't alert on a single missed packet (normal BLE/RF flakiness).
  - Want ~3-4 consecutive misses (~15-20s of silence) before treating it as a
    real loss-of-contact event.
  - **Multi-receiver consensus matters more than any single threshold** -
    since all 4 boards (3x ESP32-C3 sensor boards + the CYD hub) can
    independently listen, only escalate to an actual alert when most/all
    receivers lose the tag simultaneously. A single receiver's dropout could
    just be a local RF null near that one antenna.
  - A steadily weakening RSSI trend before going silent looks like "actually
    drifting away" (real event); a strong signal that suddenly vanishes for
    one reading and reappears looks like transient interference.
  - All of this needs real sea-trial calibration - antenna placement, hull
    material, and the RF environment on an actual boat will behave
    differently than a bench test.
  - **Update (2026-07-23): the multi-receiver consensus principle above is
    no longer just a reasoned guess - it's empirically confirmed**, on a
    bench setup (not yet a real boat), with real numbers. See section 11.6.

---

## 7. Local ring command + button-press (active GATT, not passive scanning)

**All testing in this section is against real MiLi MiTag hardware**
(specifically Kyuubi, Nothing Much, Nothing Much 2 - see section 5.1 for
their identities). Several of these findings are explicitly flagged as gaps
in *this product's* firmware versus what the spec calls for - don't assume
they generalize to other FMDN-compliant tags/brands without testing.

Everything above is passive - just listening to advertisements. Making a tag
beep, or reacting to its button being pressed, needs an *active* GATT
connection instead, which is new protocol surface. Findings:

- **Ring command is real and works fully offline.** Per the Find Hub Network
  Accessory Specification: connect (plain, unauthenticated at the BLE link
  layer), **read** the "Beacon Actions" characteristic
  (`FE2C1238-8366-4814-8EB0-01DE32100BEA`, under the Fast Pair Service
  `0xFE2C` - the *only* characteristic FMDN uses for every operation) to get
  `[protocol_version(1)][nonce(8)]`, then **write** back
  `[data_id=0x05][data_length=4][auth_key(8)][additional_data(4): ring_op,
  timeout(2), volume]`, where `auth_key` is the first 8 bytes of
  `HMAC-SHA256(ring_key, version || nonce || data_id || data_length ||
  additional_data)`. `ring_key = SHA256(EIK || 0x02)[:8]` - same derivation
  family as Recovery (`||0x01`) and unwanted-tracking (`||0x03`) keys, cheap
  (plain SHA256, no curve math). **Confirmed working against all 3 real
  tags** (Kyuubi, Nothing Much, Nothing Much 2) - genuine local beeps, zero
  cloud involvement.
- **The tags ARE connectable** while broadcasting their normal FMDN frames -
  don't trust the DIY `ESP32Firmware/main/main.c` reference in
  GoogleFindMyTools on this point, it explicitly advertises non-connectable
  (`ADV_TYPE_NONCONN_IND` / `BLE_GAP_CONN_MODE_NON`), but that's just because
  its author didn't implement Ring, not because real commercial tags work
  that way.
- **Gotcha that caused real connect failures**: FMDN tags advertise with a
  private/random BLE address (rotates roughly every ~15-20 minutes,
  independent of the 1024s EID rotation), not a public one.
  `BLEClient::connect()` defaults to assuming `BLE_ADDR_TYPE_PUBLIC` if you
  don't pass the type explicitly - silently fails with a generic "Unknown
  ESP_ERR error" otherwise. Fix: capture `advertisedDevice.getAddressType()`
  at scan time alongside the address, and pass both to `connect()`.
- **The Ring Key authentication is NOT actually enforced** by this tag's
  firmware (empirically confirmed): connecting to Kyuubi and sending a Ring
  command whose HMAC was computed with **Nothing Much's** key instead of
  Kyuubi's own still made it beep. This is a real gap in MiLi's
  implementation, not evidence the *protocol design* is flawed - the spec
  clearly intends this to be authenticated, this product just doesn't check
  it. Practical upshot: the hub can ring any nearby MiTag it can connect to
  without needing its correct key at all. Don't assume this generalizes to
  other GATT operations (untested) or other tag brands.
- **Button-press detection works and does NOT need an active ring at all** -
  this reverses what the spec fetch originally suggested (that button state
  only shows up as "ring stopped early"). Just connect and subscribe to
  notifications on the Beacon Actions characteristic, no Ring command
  required. A real notification is
  `[data_id=0x05][data_length=12][12-byte payload]` (NOT the flat 4-byte
  `[ringing_state][components][timeout]` struct the initial spec fetch
  suggested - that guess was wrong on two counts: there's a
  `[data_id][data_length]` header before the payload that was being silently
  truncated away, and reading byte 0 as "state" was actually just reading
  the echoed Data ID). The two payload bytes that matter:
  - **`payload[8] == 0x03` is the real-time button-press event.** Fires the
    instant a press happens, whether or not a ring is currently active, and
    (see below) appears to also mute an actively-ringing speaker as a side
    effect of the same press.
  - **`payload[8] == 0x02` is the ring *session* formally ending, always at
    the full originally-requested duration (~10s in testing), regardless of
    whether the audio was already silenced early.** These are decoupled: in
    one clean test, a press at ~1.9s immediately silenced the physical
    beeping (confirmed by ear), producing a `0x03` at that moment - but a
    `0x02` notification still showed up later at ~10s anyway, same timing as
    every test where the ring played out its full natural course untouched.
    So a dismissed ring's "stop" bookkeeping event doesn't fire early just
    because the button already cut the sound.

  Confirmed via several controlled tests: a deliberate multi-press pattern
  lining up cleanly against notification timestamps; a full 30-second
  listen with **zero** presses producing **zero** notifications (rules out
  spontaneous/periodic chatter on this characteristic); and a single early
  press (~1.9s into a ring) that audibly silenced the tag immediately while
  the `0x02` session-end notification still only arrived at the normal ~10s
  mark. Earlier interpretations along the way (that `0x02` meant "button
  press", then that presses during an active ring don't register at all)
  were both wrong and got corrected as more controlled data came in - this
  final model is the one that's consistent with everything observed.

  Good news for the MOB use case: a genuine always-on-while-connected "SOS
  button" is real, not just a ring-interrupt side effect - though
  maintaining a permanent GATT connection per tracked tag has its own
  cost/complexity trade-offs worth thinking through separately (one
  persistent connection per tag vs. the existing passive-scan-many-tags-at-
  once model used for EID resolution).

---

## 8. Toolchain / environment notes (Windows-specific gotchas hit along the way)

- PlatformIO is installed via the VS Code extension but `pio` isn't on the
  Bash `PATH` - full path: `C:\Users\paulr\.platformio\penv\Scripts\pio.exe`.
- `Firmware metrics can not be shown` / `UnicodeEncodeError: 'charmap' codec
  can't encode...` during flashing: PlatformIO's progress-bar printer hits a
  Windows console codepage issue. `chcp 65001` does **not** fix this from Git
  Bash, because Git Bash runs in `mintty`, which doesn't use the Win32 console
  API that `chcp` affects. Fix: `export PYTHONIOENCODING=utf-8` before running
  `pio` (forces Python's own I/O encoding regardless of console detection).
  With larger firmware (BLE+WiFi combined) this isn't just cosmetic - it can
  genuinely wedge the upload by blocking the process's stdout pipe.
- `pio device monitor`'s interactive keyboard input (typing a timestamp, etc.)
  **does not work when run from Git Bash/mintty** - PlatformIO's monitor reads
  keystrokes via a Windows console API mintty doesn't properly emulate. Run
  `pio device monitor` from a real **PowerShell** or `cmd.exe` window instead
  when the firmware needs interactive input.
- Combining BLE + WiFi + HTTPClient (with its TLS stack) exceeds the default
  ~1.3MB app partition on a 4MB flash board. Fix: `board_build.partitions =
  huge_app.csv` in `platformio.ini` (trades away OTA/most of the filesystem
  partition for a ~3MB single app partition - fine since this test doesn't
  need either).
- For bench-testing convenience (getting a timestamp without typing one in
  manually), the firmware does a plain HTTP GET to
  `http://www.gstatic.com/generate_204` and reads the `Date:` response header
  (every HTTP response has one - no JSON API dependency, no TLS needed,
  Google's infrastructure is far more reliable than small free "time API"
  services which turned out to be flaky). Requires
  `http.collectHeaders(...)` to actually capture that header, and a small
  hand-rolled RFC 7231 date parser + civil-date-to-epoch conversion (Howard
  Hinnant's `days_from_civil` algorithm) since this toolchain's libc support
  for `strptime`/`timegm` wasn't something worth gambling on. **This is bench
  convenience only** - the real deployment has no internet at sea and should
  get real time from the sensor boards' GPS PPS lines instead (not yet
  implemented).

---

## 9. Dead ends / deliberately not pursued further

- **Building a from-scratch "Seeker" to inject our own EIK** into an
  off-the-shelf tag (bypassing Google's app entirely): technically mapped out
  (would need the Fast Pair anti-spoofing *public* key for the exact tag
  model, obtainable only by intercepting real app/API traffic), but made
  moot once the account-backup-vault extraction method (GoogleFindMyTools)
  turned out to just work directly. Not pursued further.
- **"Share item location" link mechanism** - hoped the shareable link might
  leak the EIK or a useful derived key. Traced the redirect chain
  (`findhub.app.google/<slug>` -> `.../s?...` -> an **Android App Link**,
  `google.com/android/find/about/device/2?sharingInvitation=token`) but hit a
  wall: it's designed to be intercepted by the real Find Hub Android app, and
  falls through to a dead 404 in any generic desktop/headless browser context
  (even a real browser, not just plain `curl`). Going further would need
  either real Android `adb`/network monitoring on an actual phone, or
  decompiling the Find Hub APK - shelved as not worth the effort given the
  account-vault method already works cleanly.

---

## 10. Code in this directory

- `platformio.ini` - board/platform config, targets `denky32`/`esp32dev`
  (matches `TheHub/ESP32StandAloneHub/StandAloneHub`'s board choice, since
  that's the real intended hub hardware). Defines one PlatformIO environment
  per test build below, each scoped to just its own file via
  `build_src_filter` - so which one compiles is a `-e <env>` flag, never a
  file rename. Plain `pio run` (no `-e`) builds `default_envs` -
  `full_featured` - since that's the closest thing to a real release build
  today (it's the one with actual crypto/tag resolution, and the one
  secrets.h/no_secrets.h below is wired into); this will get repointed once
  the features below are actually merged into one firmware.
- `src/continuous_scan_diagnostic.cpp` (env `continuous_scan_diagnostic`) -
  minimal diagnostic build: continuous non-blocking BLE scan, prints every
  FMDN frame raw, no crypto/WiFi at all.
- `src/wifi_persistence_test.cpp` (env `wifi_persistence_test`) - NVS-backed
  WiFi credential storage + SoftAP captive-portal first-time setup, no BLE.
- `src/display_mob_test.cpp` (env `display_mob_test`) - CYD display + RGB
  LED + touch + the MOB alarm state machine, against a simulated tag list
  (not yet real BLE/crypto). Reuses the WiFi persistence flow above.
- `src/full_featured.cpp` (env `full_featured`, and PlatformIO's **default**
  - see `default_envs` above) - the crypto/ring/stats build (crypto
  self-check, benchmark, WiFi/network-time fetch, stats web page,
  ring/button-press GATT support). Predates the NVS WiFi flow - still uses a
  hardcoded placeholder network.
- `src/serial_bridge.cpp` (env `serial_bridge`) - minimal BLE-scan-to-serial
  bridge, no crypto/WiFi/display: prints one `SEEN <eid> <rssi>` line per
  FMDN sighting for a Pi-side script
  (`../OpenPlotter/eid_resolver/serial_bridge.py`) to forward into the
  resolver's `/sightings` endpoint - lets this board act as a scanning node
  over a plain USB cable. See section 11.3 for the heap-exhaustion crash
  this build's first version hit and how it was fixed.
- `src/secrets.h` - real tag EIKs for local bench testing (used only by
  `full_featured.cpp`, via `__has_include`). **Gitignored, never
  committed.** If absent, `full_featured.cpp` falls back to including
  `src/no_secrets.h` instead - the same `KNOWN_TAGS[]` structure, just
  empty, so the build (including a from-source build of the eventual public
  release) still compiles and runs without it, it just won't recognize any
  tags until you copy `no_secrets.h` to `secrets.h` and fill in real EIKs.

To compile/flash (PowerShell needed for the monitor step specifically - see
section 7):
```powershell
cd D:\Owncloud.cl\Electronics\OpenBoat\ManOverBoard\ESP32
$env:PYTHONIOENCODING="utf-8"
C:\Users\paulr\.platformio\penv\Scripts\pio.exe run -t upload
C:\Users\paulr\.platformio\penv\Scripts\pio.exe device monitor
```
That builds `full_featured` (the default). Add `-e display_mob_test`,
`-e continuous_scan_diagnostic`, `-e wifi_persistence_test`, or
`-e serial_bridge` to build one of the others instead - no renaming
involved either way. The ESP32-C3 counterpart
(`../ESP32-C3/src/serial_bridge_c3.cpp`) is a **separate PlatformIO
project** (different chip, different board id) - see section 11.3.

---

## 11. OpenPlotter multi-scanner MOB relay (2026-07-22/23 session)

Everything below happened on the OpenPlotter side
(`../OpenPlotter/eid_resolver/`) and on a real Raspberry Pi deployment -
see that project's own README.md for setup/usage. Recorded here because
several of these were genuine bugs with root causes worth not
re-discovering blind next time.

### 11.1 Device-time offset never ported to OpenPlotter

`candidates.py` originally generated candidate EIDs assuming the tag's
internal clock equals real UTC - but section 5 of this document already
established that's false. Known-tag matching silently never worked on the
OpenPlotter side, even with the correct EIK and a physical tag sitting
right next to the Pi, because the candidate window was centered on the
wrong point in time. Fixed by adding a `device_time_offset_secs` field per
tag (see the OpenPlotter README's "Setup" section for the current
`known_tags.json` schema) and generating candidates at
`real_ts - device_time_offset_secs`.

Re-deriving a tag's offset live (brute-forcing against a real Pi sighting,
cheap on a real CPU unlike the ESP32's ~2.1s/candidate) turned up a
follow-on subtlety: the offset is only accurate to about one rotation
window, not the exact second, because it's reconstructed from *when a
sighting was recorded* (scan + HTTP-post latency) rather than the tag's
true window boundary - re-deriving the same tag's offset twice ~20 minutes
apart landed exactly one `ROTATION_PERIOD` (1024s) apart both times.
`candidates.py` now searches a small `OFFSET_TOLERANCE_WINDOWS` band
around the stored offset (deliberately narrower than the ESP32's own
`WIDE_SEARCH_WINDOWS = ±5` - see section 3's mbedTLS gotchas - because
this project's 3 real tags' offsets are only 1-2 windows apart from each
other, so a wide band would make them indistinguishable rather than just
tolerant of jitter).

### 11.2 Known-tag matching pegged a real Pi's CPU at 99.9%

Immediately after fixing 11.1, tags matched once right after a server
restart, then all looked "offline" a few minutes later - looked like the
crypto/keys were wrong again, but wasn't. Root cause: the per-sighting
match function rebuilt a full 24h-forward candidate table (~84 windows x
3 known tags x offset-tolerance band) on **every single BLE sighting**,
including every ambient device nearby, not just known tags. This pegged a
real Pi's CPU at 99.9% for 3+ hours straight and made it fall permanently
behind real time, so every sighting after the first got checked against a
window that had already passed by the time it was processed.

Fixed with a small per-tag `KnownTagCache` that only recomputes its
current-window candidate set when real time actually crosses into a new
rotation window (~every 17 minutes), not on every sighting - per-sighting
matching became a plain set lookup (~0.4µs) instead of fresh EC crypto
every time. **Lesson for next time matching silently stops working: check
`ps aux`/CPU load before assuming the crypto or keys are wrong** - a
CPU-starved matching loop looks identical to "not matching" from the
dashboard's perspective.

### 11.3 CYD/C3 as USB-serial scanners, and a real arduino-esp32 crash bug

Added minimal firmware (`src/serial_bridge.cpp` here, and
`../ESP32-C3/src/serial_bridge_c3.cpp` as a separate PlatformIO project
for the C3 - different chip, needs `ARDUINO_USB_MODE=1`/
`ARDUINO_USB_CDC_ON_BOOT=1` since the C3 has no separate USB-UART bridge
chip, talks Serial over its own native USB-CDC peripheral instead) that
just scans for FMDN broadcasts and prints `SEEN <eid> <rssi>` over USB
serial, for a Pi-side Python script to forward into the OpenPlotter
resolver's `/sightings` endpoint. Lets a board act as a scanning node with
no WiFi/network stack of its own.

**Real crash bug found on real hardware, traced into the actual
arduino-esp32 BLE library source, not our code**: the first version (using
the library's high-level `haveServiceData()`/`getServiceDataUUID()`/
`getServiceData()` accessors, default `shouldParse=true`) crashed the C3
within ~30-40s: `Failed to allocate N bytes for payload in copy
constructor` (`BLEAdvertisedDevice.cpp`), then `abort()`/reboot. Confirmed
by reading `framework-arduinoespressif32`'s `BLEScan.cpp`/
`BLEAdvertisedDevice.cpp` directly: `onResult()` receives a full
**by-value copy** of every single advertisement in range (not just FMDN
ones - a real RF environment showed ~50+ raw advertisements/sec from
unrelated nearby devices, confirmed via a `btmon` HCI capture on the Pi),
and `shouldParse=true` additionally builds a `std::vector<String>` of
every service-data entry plus Strings for name/manufacturer-data/etc. per
packet - heavy heap churn on top of the one payload-copy malloc that
happens regardless. The C3 has meaningfully less free heap than the
classic dual-core ESP32 (this project's CYD board) after the BLE
stack/Arduino runtime/native-USB driver take their share, so it fragments
and dies far faster - **the CYD hadn't shown this symptom over a 35-minute
run at the time, but has the identical underlying risk with more headroom,
not immunity** (confirmed later: both boards ran crash-free for 6+ hours
straight after the fix below).

Fix, applied to **both** boards as cheap insurance (not just the one that
had actually crashed): `setAdvertisedDeviceCallbacks(cb, true,
/*shouldParse=*/false)`, plus a hand-rolled `extract_fmdn_eid()` that
scans the raw advertisement payload bytes directly for the one AD
structure needed (Service Data, 16-bit UUID 0xFEAA) via
`getPayload()`/`getPayloadLength()`, instead of the library's per-field
parsing.

Remotely reflashing over SSH (no need for the board to be physically
connected to the dev machine): copy the PlatformIO-built `bootloader.bin`,
`partitions.bin`, `firmware.bin` plus the static
`boot_app0.bin` (bundled with the framework at
`framework-arduinoespressif32/tools/partitions/boot_app0.bin`, doesn't
change per-project) to the Pi, then run `esptool` (`pip install esptool`
into the venv - hits Debian's PEP 668 externally-managed-environment
restriction if you try a bare system `pip install`, use the venv) with
the exact same offsets PlatformIO uses (get them via
`pio run -t upload --upload-port <anything> -v`, which prints the full
command before failing if the port doesn't actually exist locally):
`0x1000` bootloader, `0x8000` partitions, `0xe000` boot_app0, `0x10000`
firmware.

### 11.4 bleak/BlueZ silently deduplicating advertisements (Pi onboard scanner)

The Pi's own onboard-Bluetooth scanner (`ble_scanner.py`, via `bleak`) was
seeing gaps up to ~20s between sightings, which looked like a hardware/
antenna problem. A raw `btmon` HCI capture on the Pi proved the radio
itself was fine (continuous reception, max gap 0.124s across 25s / ~1400
raw advertisements) - the bug was one layer up. `bleak` (confirmed on
v3.0.2) explicitly sets BlueZ's `SetDiscoveryFilter` `DuplicateData` to
**False**, overriding BlueZ's own default of `True`. Since these tags
broadcast the *same* 20-byte EID for their whole ~1024s rotation window,
BlueZ was deduplicating almost every repeat before `bleak` ever saw it.
Fixed in `ble_scanner.py`: `BleakScanner(..., bluez=dict(filters=dict(DuplicateData=True)))`.
Dropped the onboard scanner's max gap from ~20s to 6-8s in isolation.

Separately, the Pi's onboard Bluetooth chip turned out to share hardware
with an active WiFi hotspot (`wlan9`, an OpenPlotter feature for other
devices/apps to connect to - confirmed via `wlan0`/`wlan9` both resolving
to the same `mmc1:0001:1` SDIO device path in `/sys/class/net/*/device`,
i.e. the same physical Broadcom/Cypress combo chip, not a separate USB
adapter). Disabling that hotspot (`nmcli connection down
OpenPlotter-Hotspot`) improved the onboard scanner's worst-case gap by a
further ~20-30% (14→12s, 20→14s, etc. across the three tags) -
confirming real WiFi/BT coexistence contention, though the onboard
scanner remained the weakest of the three scanners even with WiFi fully
off, so hardware/antenna quality is still the dominant remaining factor,
not the sole one.

### 11.5 Per-source reception stats

`brain.py`'s combined (any-source) gap stats mix every scanner's
sightings into one sequence per tag, which hides exactly the comparison
the multi-scanner exercise below needs. Added `SourceStats`/
`TrackedTag.per_source` (min/avg/max gap tracked independently per scanner
`source` tag) and a second table on `/debug` breaking it out - this is
what actually let the different scanners be compared against each other
rather than just guessing from the mixed view.

### 11.6 Multi-scanner reception comparison - empirical confirmation of section 6's design principle

With real physical tags near a real Pi, comparing the onboard Bluetooth
scanner, a CYD wired in via serial, and an ESP32-C3 wired in via serial,
all feeding the same `brain.py` pipeline simultaneously:

| # scanners | worst-case *combined* max gap (whichever scanner catches a beacon first resets the clock) |
|---|---|
| 1 (onboard alone) | 12-20s |
| 2 (+ CYD) | 6.0s (confirmed over 34min and 45min runs) |
| 3 (+ ESP32-C3) | 4.0s (short runs) / 4-6s (6-hour run) |

Confirmed over both short (~30-45 min) runs and one 6-hour unattended run
(WiFi hotspot off, per 11.4) - the 6-hour run's slightly higher worst-case
(4-6s vs. a clean 4.0s on shorter runs) is expected, not concerning: far
more total sighting windows sampled means a higher chance of eventually
catching a rare moment where all active scanners have a bad beat
simultaneously, not evidence the shorter runs were a fluke.

All three scanner processes (server + 2x `serial_bridge.py`) and both
reflashed ESP32 firmwares ran the full 6 hours with **zero crashes,
zero reconnects, zero restarts** - real confirmation the 11.3 crash fix
holds up under sustained real-world load, not just a quiet few minutes.

This is the first real empirical data behind section 6's "multi-receiver
consensus matters more than any single threshold" design principle - it
was a reasoned guess before, it's a measured result now (on a bench setup,
not yet a real boat - RF conditions on an actual hull with engine/rigging
in the way will differ).

Rough modeling (back-calculating each scanner's per-broadcast catch rate
from its average gap, assuming independence across scanners) suggests a
4th scanner should push the reliable ceiling down further, plausibly to a
consistent 4s - but the actual 6-hour data already shows *more* double-miss
events than a pure-independence model predicts, meaning there's real
correlation between scanners' misses (most likely shared environmental
conditions affecting a tag's reception at a given moment, not fully
independent per-scanner randomness) that the simple model doesn't capture.
Worth treating any further scanner-count extrapolation as a hypothesis to
test, not a number to trust blind - consistent with how every other claim
in this section was actually verified against real hardware rather than
assumed.

### 11.7 Chained/looped UART relay - design considerations, not yet built

Extensive design-only discussion (no code written) around relaying BLE
sightings between multiple ESP32-C3s over a daisy-chained or ring-shaped
UART link, for a future real-boat topology (many scanners, one WiFi
uplink). Not implemented or tested - recorded here as a pointer in case
this gets picked up later, full reasoning lives in that session's
transcript rather than duplicated here:

- **Bandwidth is a non-issue either way.** Naive full relay (every node
  forwards everything, no dedup) needs ~11.5kbps worst-case for a 10-node
  chain/5 tags/2s-interval scenario - comfortably covered by standard UART
  baud rates even over 2m per-hop cable runs. A dedup optimization
  (suppress forwarding a stale relayed report once you've caught the same
  tag yourself) collapses this further to ~1.3kbps, independent of chain
  length - but only applies if you don't need each node's individual RSSI
  reading; wanting per-node RSSI (for positional information) reverts you
  to the naive numbers, still trivially low.
- **A ring (not just a one-way chain) with bidirectional relay is free
  bandwidth-wise** (UART TX/RX are already independent full-duplex
  channels) and buys real fault tolerance - a single dead node or broken
  cable no longer isolates everything downstream of it, since the other
  direction still reaches the bridge.
- **EMI/loop-area concerns for a closed power+ground ring**: mutual
  coupling between a large loop and a spatially compact noise source
  (e.g. an alternator) actually *decreases* with increasing loop radius
  (M ∝ 1/R for coaxial loops of very different size, not proportional to
  the large loop's area) - a correction to an initial assumption that
  bigger enclosed loop area straightforwardly means more EMI pickup, which
  isn't true for this specific near-field-compact-source geometry. Ground
  potential differences (a different, conductive-coupling mechanism, not
  magnetic) remain a separate concern differential signaling (RS-422/CAN)
  would address better than shielding alone.
- **CRC over the message, not UART parity** - parity only detects (not
  corrects) single-bit errors per byte; a CRC-8/16 per message plus
  discard-on-mismatch is simpler and sufficient given the periodic
  (~2s) redundant reporting nature of this system already self-heals at
  the application level - true forward error correction (Hamming/
  Reed-Solomon) would be solving a problem this design doesn't actually
  have.
- **RS-422 (full-duplex, matches the point-to-point per-hop topology
  directly, no DE/RE bus-arbitration complexity RS-485 needs) over RS-485**
  if going the differential-signaling route. Candidate chips researched:
  MAX3488, ADM3491/3490, LTC2863IS8-2#PBF (confirmed full-duplex, 3.0-5.5V,
  ~$8), ISL83488/ISL83490 (Renesas/Intersil, confirmed full-duplex,
  3.0-3.6V, ~$1.50, no published ESD rating in either's datasheet - the
  ISL83488 specifically is slew-rate-limited, i.e. lower max speed
  (250kbps, still far more than needed) in exchange for reduced EMI
  emission and better tolerance of imperfect termination, arguably the
  better fit of the two given this project has zero speed pressure and
  real EMI concerns). CAN bus (native TWAI controller already on the C3,
  external transceiver ~$1) also considered - gets CRC-15 and bus
  arbitration for free as native protocol features, worth a serious look
  if a future revision wants shared-bus rather than point-to-point wiring.
