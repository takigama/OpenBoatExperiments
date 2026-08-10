// Singleton test, stage 2: after proving the generate_eid() crypto port is
// correct (stage 1, against a synthetic test vector), this stage scans real
// BLE advertisements and tries to resolve the real MiTag using its actual
// extracted EIK - recomputing the expected identifier for the current 1024s
// rotation window on every scan pass, so it keeps resolving correctly across
// a live re-key (not just a single snapshot match).
//
// Real over-the-air FMDN advertisement layout (from GoogleFindMyTools'
// ESP32Firmware/main/main.c, which builds this same frame from the
// accessory side):
//   Service Data (AD type 0x16), 16-bit UUID 0xFEAA, then:
//     byte 0:     frame type (0x40 or 0x41)
//     bytes 1-20: 20-byte ephemeral identifier
//     byte 21:    hashed flags (optional, ignored here)

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <Arduino.h>
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>

// Wraps the real Serial to prefix every line with a "[t=<ms since boot>] "
// timestamp, so every log line (not just the ones inside test loops that
// explicitly print one) can be correlated against a single shared clock -
// covers scan matches, connect/ring/notify logs, everything. Overriding
// write() means print()/println()/printf() all get this for free, since
// they're implemented in terms of it by the Print base class. The #define
// below only affects this translation unit (full_featured.cpp) - other libraries'
// own .cpp files still reference the real Serial object directly.
class TimestampedSerial : public Print {
public:
    void begin(unsigned long baud) { Serial.begin(baud); }
    int available() { return Serial.available(); }
    int read() { return Serial.read(); }

    size_t write(uint8_t c) override {
        if (at_line_start) {
            at_line_start = false;
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "[t=%8lu] ", millis());
            Serial.write((const uint8_t *)buf, n);
        }
        size_t ret = Serial.write(c);
        if (c == '\n') at_line_start = true;
        return ret;
    }

private:
    bool at_line_start = true;
};

static TimestampedSerial TSerial;
#define Serial TSerial

// Fast Pair Service (0xFE2C) - the single GATT service FMDN operations
// (provisioning, ring, unwanted-tracking) all live under, per the Find Hub
// Network Accessory Specification.
static const BLEUUID FAST_PAIR_SERVICE_UUID((uint16_t)0xFE2C);
// "Beacon Actions" characteristic - the only characteristic FMDN uses. A
// read returns [protocol_version(1)][nonce(8)]; writes carry a Data ID
// (0x05 = Ring) plus an HMAC computed with the Ring Key.
static const BLEUUID BEACON_ACTIONS_CHAR_UUID("FE2C1238-8366-4814-8EB0-01DE32100BEA");

static const int K = 10;               // rotation period exponent
static const uint32_t ROTATION_PERIOD = 1UL << K; // 1024s

// This file predates the NVS + SoftAP config-portal WiFi flow in
// wifi_persistence_test.cpp / display_mob_test.cpp - it still uses a
// hardcoded network for its own bench-only network-time fetch (see
// try_get_time_from_network() below). Not a real secret, just a
// placeholder - fill in your own to build this environment
// (`pio run -e full_featured`), or better, port it to the NVS-based flow.
static const char *WIFI_SSID = "your-wifi-ssid";
static const char *WIFI_PASSWORD = "your-wifi-password";

// Real extracted EIKs (from GoogleFindMyTools) live in secrets.h, which is
// gitignored - see no_secrets.h for the template. These are genuine
// secrets, not placeholders: anyone holding one of these EIKs can compute
// that specific tag's rotating identifier.
struct KnownTag {
    const char *name;
    uint8_t eik[32];
};

#if __has_include("secrets.h")
#include "secrets.h" // real tag EIKs, gitignored
#else
#include "no_secrets.h" // same KNOWN_TAGS[] structure, just empty
#endif

static const int NUM_KNOWN_TAGS = sizeof(KNOWN_TAGS) / sizeof(KNOWN_TAGS[0]);

// --- Sighting statistics, for the stats web page ---
// Tracks, per tag, how long it's actually been since we last heard from it,
// and running min/avg/max gaps between sightings - lets us empirically see
// what a "normal" gap looks like before picking a MOB alert threshold,
// rather than guessing at a number.
struct SightingStats {
    bool ever_seen = false;
    uint32_t last_seen_unix = 0;
    uint32_t count = 0;
    uint32_t sum_gap_seconds = 0;
    uint32_t min_gap_seconds = 0xFFFFFFFFUL;
    uint32_t max_gap_seconds = 0;
    int last_rssi = 0;
};

// Records a new sighting at real wall-clock time `now` (current_unix_time(),
// NOT the tag's own unsynced device-time) - gap stats are only meaningful
// against a real, consistent clock.
static void record_sighting(SightingStats &stats, uint32_t now, int rssi) {
    if (stats.ever_seen) {
        uint32_t gap = (now >= stats.last_seen_unix) ? (now - stats.last_seen_unix) : 0;
        stats.sum_gap_seconds += gap;
        if (gap < stats.min_gap_seconds) stats.min_gap_seconds = gap;
        if (gap > stats.max_gap_seconds) stats.max_gap_seconds = gap;
    }
    stats.ever_seen = true;
    stats.last_seen_unix = now;
    stats.last_rssi = rssi;
    stats.count++;
}

static SightingStats g_known_stats[NUM_KNOWN_TAGS];

// Small fixed-size table for EIDs that don't match any known tag - not
// trying to track identity across a rotation boundary (would need the
// EIK we don't have), just within-window repeat sightings. Evicts the
// least-recently-seen slot when full.
static const int UNKNOWN_TABLE_LEN = 8;
struct UnknownSlot {
    bool used = false;
    uint8_t eid[20];
    SightingStats stats;
};
static UnknownSlot g_unknown_table[UNKNOWN_TABLE_LEN];

static void record_unknown_sighting(const uint8_t eid[20], uint32_t now, int rssi) {
    int free_idx = -1;
    int oldest_idx = 0;
    uint32_t oldest_time = 0xFFFFFFFFUL;

    for (int i = 0; i < UNKNOWN_TABLE_LEN; i++) {
        if (g_unknown_table[i].used && memcmp(g_unknown_table[i].eid, eid, 20) == 0) {
            record_sighting(g_unknown_table[i].stats, now, rssi);
            return;
        }
        if (!g_unknown_table[i].used && free_idx < 0) free_idx = i;
        if (g_unknown_table[i].used && g_unknown_table[i].stats.last_seen_unix < oldest_time) {
            oldest_time = g_unknown_table[i].stats.last_seen_unix;
            oldest_idx = i;
        }
    }

    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    g_unknown_table[idx].used = true;
    memcpy(g_unknown_table[idx].eid, eid, 20);
    g_unknown_table[idx].stats = SightingStats(); // fresh stats for this slot
    record_sighting(g_unknown_table[idx].stats, now, rssi);
}

// --- Stage 1 self-check vector (kept as a boot-time regression check) ---
static const uint8_t TEST_KEY[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint32_t TEST_TIMESTAMP = 1700000000UL;
static const char *EXPECTED_EID_HEX = "6f3bcc7d38665e6cadf7ca48e9ce6d3ea3942d83";

// SECP160r1 (SEC2) domain parameters - pulled from the Python `ecdsa` library
// directly (see conversation), not transcribed from memory.
static const char *SECP160R1_P  = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFF";
static const char *SECP160R1_A  = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFC";
static const char *SECP160R1_B  = "1C97BEFC54BD7A8B65ACF89F81D4D4ADC565FA45";
static const char *SECP160R1_GX = "4A96B5688EF573284664698968C38BB913CBFC82";
static const char *SECP160R1_GY = "23A628553168947D59DCC912042351377AC5FB32";
static const char *SECP160R1_N  = "0100000000000000000001F4C8F927AED3CA752257";

// Persistent crypto context - the curve group and RNG are loaded/seeded once
// in setup() and reused across every generate_eid() call, instead of paying
// entropy-gathering + group-loading cost per BLE advertisement seen.
static mbedtls_ecp_group g_grp;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;

static int load_secp160r1(mbedtls_ecp_group *grp) {
    int ret;
    if ((ret = mbedtls_mpi_read_string(&grp->P, 16, SECP160R1_P)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->A, 16, SECP160R1_A)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->B, 16, SECP160R1_B)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->G.X, 16, SECP160R1_GX)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->G.Y, 16, SECP160R1_GY)) != 0) return ret;
    if ((ret = mbedtls_mpi_lset(&grp->G.Z, 1)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->N, 16, SECP160R1_N)) != 0) return ret;
    grp->pbits = mbedtls_mpi_bitlen(&grp->P);
    grp->nbits = mbedtls_mpi_bitlen(&grp->P);
    return 0;
}

static int crypto_init() {
    mbedtls_ecp_group_init(&g_grp);
    int ret = load_secp160r1(&g_grp);
    if (ret != 0) return ret;

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    return mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy, NULL, 0);
}

static void build_and_encrypt_r_dash(const uint8_t identity_key[32],
                                      uint32_t masked_timestamp,
                                      uint8_t r_dash_out[32]) {
    uint8_t ts_bytes[4] = {
        (uint8_t)(masked_timestamp >> 24),
        (uint8_t)(masked_timestamp >> 16),
        (uint8_t)(masked_timestamp >> 8),
        (uint8_t)(masked_timestamp)};

    uint8_t data[32];
    memset(data, 0xFF, 11);
    data[11] = (uint8_t)K;
    memcpy(&data[12], ts_bytes, 4);
    memset(&data[16], 0x00, 11);
    data[27] = (uint8_t)K;
    memcpy(&data[28], ts_bytes, 4);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, identity_key, 256);
    // mbedtls_aes_crypt_ecb only does ONE 16-byte block per call - the data
    // structure is 32 bytes (2 blocks), so this needs two calls.
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, data, r_dash_out);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, data + 16, r_dash_out + 16);
    mbedtls_aes_free(&aes);
}

// Uses the persistent g_grp/g_ctr_drbg context set up in crypto_init().
static int generate_eid(const uint8_t identity_key[32], uint32_t timestamp,
                         uint8_t eid_out[20]) {
    uint32_t masked_timestamp = timestamp & ~(ROTATION_PERIOD - 1UL);

    uint8_t r_dash[32];
    build_and_encrypt_r_dash(identity_key, masked_timestamp, r_dash);

    mbedtls_mpi r_dash_mpi, r;
    mbedtls_mpi_init(&r_dash_mpi);
    mbedtls_mpi_init(&r);

    int ret = mbedtls_mpi_read_binary(&r_dash_mpi, r_dash, 32);
    if (ret == 0) ret = mbedtls_mpi_mod_mpi(&r, &r_dash_mpi, &g_grp.N);

    mbedtls_ecp_point R;
    mbedtls_ecp_point_init(&R);

    if (ret == 0) {
        ret = mbedtls_ecp_mul(&g_grp, &R, &r, &g_grp.G, mbedtls_ctr_drbg_random, &g_ctr_drbg);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&R.X, eid_out, 20);
    }

    mbedtls_ecp_point_free(&R);
    mbedtls_mpi_free(&r_dash_mpi);
    mbedtls_mpi_free(&r);

    return ret;
}

// Ring key = SHA256(EIK || 0x02), truncated to first 8 bytes - same
// derivation family as the Recovery (0x01) and unwanted-tracking (0x03) keys
// documented in GoogleFindMyTools' key_derivation.py, just a different
// suffix byte. Much cheaper than generate_eid() - plain SHA256, no curve
// math at all.
static void derive_ring_key(const uint8_t eik[32], uint8_t ring_key_out[8]) {
    uint8_t buf[33];
    memcpy(buf, eik, 32);
    buf[32] = 0x02;

    uint8_t digest[32];
    mbedtls_sha256(buf, sizeof(buf), digest, 0 /* 0 = SHA-256, not SHA-224 */);
    memcpy(ring_key_out, digest, 8);
}

// First 8 bytes of HMAC-SHA256(ring_key, message) - the "one-time
// authentication key" the ring command's payload requires.
static void hmac_sha256_truncated8(const uint8_t *key, size_t key_len,
                                    const uint8_t *msg, size_t msg_len,
                                    uint8_t out8[8]) {
    uint8_t full[32];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md_info, key, key_len, msg, msg_len, full);
    memcpy(out8, full, 8);
}

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) Serial.printf("%02x", buf[i]);
}

static bool self_check() {
    uint8_t eid[20];
    if (generate_eid(TEST_KEY, TEST_TIMESTAMP, eid) != 0) return false;

    char computed_hex[41];
    for (int i = 0; i < 20; i++) sprintf(&computed_hex[i * 2], "%02x", eid[i]);
    computed_hex[40] = '\0';

    return strcmp(computed_hex, EXPECTED_EID_HEX) == 0;
}

// Times generate_eid() on this actual hardware, then extrapolates how long a
// full brute-force search would take across every possible 1024s window
// since a "tags probably didn't exist before this" cutoff - answers "how
// long would the CYD take to find an unknown tag's device-time offset from
// scratch" for the real, offline deployment.
static void benchmark_generate_eid() {
    const int N = 200;
    uint8_t dummy[20];
    uint32_t t0 = 1700000000UL;

    Serial.printf("Running generate_eid() benchmark (%d iterations, no progress = still working)...\n", N);

    unsigned long start_us = micros();
    for (int i = 0; i < N; i++) {
        generate_eid(KNOWN_TAGS[0].eik, t0 + (uint32_t)i * ROTATION_PERIOD, dummy);
        if ((i + 1) % 20 == 0) {
            Serial.printf("  ...%d/%d (%lu ms elapsed)\n", i + 1, N, (micros() - start_us) / 1000UL);
        }
    }
    unsigned long elapsed_us = micros() - start_us;

    double us_per_call = (double)elapsed_us / N;
    Serial.printf("Benchmark: %d calls in %lu us (%.1f us/call, %.1f calls/sec)\n",
                  N, elapsed_us, us_per_call, 1000000.0 / us_per_call);

    const uint32_t EPOCH_2018 = 1514764800UL; // 2018-01-01 00:00:00 UTC
    const uint32_t ROUGH_NOW = 1784000000UL;  // rough "now", ~mid-2026 - only
                                               // needs to be right to within a
                                               // year or so for this estimate
    uint32_t range_seconds = ROUGH_NOW - EPOCH_2018;
    uint32_t total_windows = range_seconds / ROTATION_PERIOD;

    double total_seconds = (total_windows * us_per_call) / 1000000.0;
    Serial.printf("Full 2018-to-now search space: %lu windows -> ~%.1f sec (~%.1f min) to exhaust on this hardware\n",
                  (unsigned long)total_windows, total_seconds, total_seconds / 60.0);
}

// --- Live clock: manual entry once at boot, tracked via millis() after that ---
static uint32_t g_base_unix_time = 0;
static uint32_t g_base_millis = 0;

static uint32_t current_unix_time() {
    return g_base_unix_time + (millis() - g_base_millis) / 1000UL;
}

// Days since 1970-01-01 for a given civil (proleptic Gregorian) date.
// Howard Hinnant's well-known constant-time algorithm - avoids depending on
// strptime()/timegm() being present in this toolchain's libc at all.
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// Parses an RFC 7231 HTTP-date header, e.g. "Wed, 21 Oct 2015 07:28:00 GMT",
// into a Unix timestamp.
static uint32_t parse_http_date_to_unix(const String &date) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = date.substring(5, 7).toInt();
    String monStr = date.substring(8, 11);
    int year = date.substring(12, 16).toInt();
    int hour = date.substring(17, 19).toInt();
    int minute = date.substring(20, 22).toInt();
    int second = date.substring(23, 25).toInt();

    int month = 1;
    for (int i = 0; i < 12; i++) {
        if (monStr == months[i]) {
            month = i + 1;
            break;
        }
    }

    long days = days_from_civil(year, month, day);
    return (uint32_t)days * 86400UL + (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + (uint32_t)second;
}

// Connects to WiFi (and stays connected - needed for the stats web server)
// and reads the Date header off a plain HTTP response (every HTTP response
// has one - no JSON API dependency, no TLS needed) to seed the clock.
// Returns 0 and fills *out_unix_time on success; nonzero (and does nothing
// else) if WiFi/the request fails, so the caller can fall back to manual
// entry.
static int try_get_time_from_network(uint32_t *out_unix_time) {
    Serial.print("Connecting to WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 10000) {
            Serial.println();
            Serial.println("WiFi connect timed out.");
            WiFi.disconnect(true);
            return -1;
        }
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());

    HTTPClient http;
    const char *headerKeys[] = {"Date"};
    http.collectHeaders(headerKeys, 1);
    http.begin("http://www.gstatic.com/generate_204");
    int code = http.GET();
    if (code != 204 && code != 200) {
        Serial.printf("Time request failed, HTTP code %d\n", code);
        http.end();
        return -1;
    }

    String dateHeader = http.header("Date");
    http.end();
    // WiFi stays connected (no disconnect here) - the stats web server needs it.

    if (dateHeader.length() < 25) {
        Serial.println("No usable Date header in the response.");
        return -1;
    }

    *out_unix_time = parse_http_date_to_unix(dateHeader);
    return 0;
}

static void prompt_for_time() {
    Serial.println();
    Serial.println("Enter the current UNIX timestamp (e.g. `date +%s` in a terminal) and press Enter:");

    // Echoes every byte back as it arrives (so you can SEE input is reaching
    // the device) and accepts either \r or \n as the line terminator (no
    // fixed timeout that could silently swallow a slow/partial paste - the
    // previous version used readStringUntil('\n'), which only recognizes
    // '\n' and gives up after 1s with no feedback if the terminal only sends
    // '\r' on Enter).
    String line = "";
    while (true) {
        if (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (line.length() > 0) break;
                continue; // ignore a stray leading \r or \n
            }
            Serial.write(c); // local echo
            line += c;
        } else {
            delay(5);
        }
    }
    Serial.println();

    g_base_unix_time = (uint32_t)line.toInt();
    g_base_millis = millis();
    Serial.printf("Received %d chars, parsed unix_time=%lu\n", line.length(), (unsigned long)g_base_unix_time);
}

// --- BLE scanning ---
static const BLEUUID FMDN_SERVICE_UUID((uint16_t)0xFEAA);

// onResult() runs inside the BLE stack's own task (BTC_TASK), which must
// stay responsive - it must NOT do the crypto (ECC scalar mult is too slow
// to run there; doing so starved the idle task long enough to trip the
// watchdog). It only does the cheap frame-format check and hands the 20-byte
// candidate off to loop() via this queue, which runs at normal app priority.
static const int PENDING_QUEUE_LEN = 8;
struct PendingEid {
    uint8_t eid[20];
    int rssi;
    BLEAddress address;
    esp_ble_addr_type_t addressType;
    uint8_t frame_type;
    bool has_hashed_flags;
    uint8_t hashed_flags;
};
static PendingEid g_pending[PENDING_QUEUE_LEN];
static volatile int g_pending_count = 0;
// onResult() (BLE stack task) and loop() (app task) can run on different
// cores concurrently - this guards the shared queue between them.
static portMUX_TYPE g_pending_mux = portMUX_INITIALIZER_UNLOCKED;

class FmdnAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!advertisedDevice.haveServiceData()) return;
        if (!advertisedDevice.getServiceDataUUID().equals(FMDN_SERVICE_UUID)) return;

        String data = advertisedDevice.getServiceData();
        if (data.length() < 21) return; // frame type (1) + EID (20)

        uint8_t frame_type = (uint8_t)data[0];
        if (frame_type != 0x40 && frame_type != 0x41) return;

        portENTER_CRITICAL(&g_pending_mux);
        if (g_pending_count < PENDING_QUEUE_LEN) {
            PendingEid &slot = g_pending[g_pending_count];
            memcpy(slot.eid, data.c_str() + 1, 20);
            slot.rssi = advertisedDevice.getRSSI();
            slot.address = advertisedDevice.getAddress();
            slot.addressType = (esp_ble_addr_type_t)advertisedDevice.getAddressType();
            slot.frame_type = frame_type;
            // Byte 21 ("hashed flags") is documented as optional/present only
            // in the 0x19-length variant of this frame - never looked at
            // until now. Checking for a double-click/beep-triggering change
            // here.
            slot.has_hashed_flags = data.length() >= 22;
            slot.hashed_flags = slot.has_hashed_flags ? (uint8_t)data[21] : 0;
            g_pending_count++;
        }
        portEXIT_CRITICAL(&g_pending_mux);
    }
};

// The tag's internal rotation counter is NOT synced to real UTC - it free-
// runs from whenever it was last provisioned/reset. Found empirically by
// brute-forcing +/-5 years of candidate windows against a real, RSSI-
// confirmed capture from this exact tag: its internal clock sits about 499
// days BEHIND real time (device_time ~= real_utc_time - this offset). Google
// papers over this for the real app by having its backend separately supply
// each report's `deviceTimeOffset` rather than assuming current time.
//
// NOTE: this drifts by ~1 real second per real second (expected - it's just
// a fixed offset from an unsynced clock, not actual skew), so it goes stale
// the longer this firmware sits between reflashes, and resets to a new value
// entirely if the tag loses power again. Was a hardcoded constant good for
// only one bench session; now mutable, self-corrected at runtime by
// try_widen_and_correct_offset() below when an "unknown" EID turns out to
// just be the most-overdue known tag drifted outside the +/-1 window.
static uint32_t g_device_time_offset_seconds = 43162624UL;

// Cached once we've resolved each tag's live address - needed to open a GATT
// connection for the ring command later, since passive scanning alone never
// needs to connect to anything. FMDN tags advertise with a private/random
// address (for privacy - rotates over time), not a public one, so the
// address TYPE has to be captured and reused too, or BLEClient::connect()
// silently assumes BLE_ADDR_TYPE_PUBLIC and fails.
// One slot per KNOWN_TAGS entry now (not just Kyuubi) - testing whether
// Nothing Much / Nothing Much 2 can connect too tells us whether "connect
// fails" is a whole-product-line non-connectable-advertising thing, or
// something specific to Kyuubi.
static BLEAddress g_cached_address[3] = {
    BLEAddress("00:00:00:00:00:00"), BLEAddress("00:00:00:00:00:00"), BLEAddress("00:00:00:00:00:00")};
static esp_ble_addr_type_t g_cached_address_type[3] = {BLE_ADDR_TYPE_PUBLIC, BLE_ADDR_TYPE_PUBLIC, BLE_ADDR_TYPE_PUBLIC};
static volatile bool g_cached_address_known[3] = {false, false, false};

// Self-healing offset drift correction. An "unknown" EID might just be the
// most-overdue known tag drifted outside the usual +/-1 window (this is
// exactly what was happening intermittently in testing - g_device_time_
// offset_seconds only reflects reality at the moment it was last refreshed,
// and drifts stale over a long session). Rather than brute-force searching
// every unknown frame against every tag (expensive - ~2.1s/candidate),
// only widen the search for whichever known tag has gone longest without a
// successful match - the one most likely to have actually drifted - and
// only out to a bounded extra range, not a full re-discovery.
static const int WIDE_SEARCH_WINDOWS = 5; // +/-5 windows ~= 85 minutes of drift tolerance

static bool try_widen_and_correct_offset(const uint8_t seen_eid[20], uint32_t real_now, int rssi,
                                          const BLEAddress &address, esp_ble_addr_type_t addressType) {
    int worst_idx = -1;
    uint32_t worst_last_seen = 0xFFFFFFFFUL;
    bool worst_never_seen = false;

    for (int i = 0; i < NUM_KNOWN_TAGS; i++) {
        if (!g_known_stats[i].ever_seen) {
            if (!worst_never_seen) {
                worst_never_seen = true;
                worst_idx = i;
            }
            continue;
        }
        if (worst_never_seen) continue; // a never-seen tag already takes priority
        if (g_known_stats[i].last_seen_unix < worst_last_seen) {
            worst_last_seen = g_known_stats[i].last_seen_unix;
            worst_idx = i;
        }
    }
    if (worst_idx < 0) return false;

    const KnownTag &tag = KNOWN_TAGS[worst_idx];
    uint32_t now = real_now - g_device_time_offset_seconds;

    for (int delta = -WIDE_SEARCH_WINDOWS; delta <= WIDE_SEARCH_WINDOWS; delta++) {
        if (delta >= -1 && delta <= 1) continue; // already covered by the normal +/-1 pass
        uint32_t t = now + (uint32_t)(delta * (int)ROTATION_PERIOD);
        uint8_t candidate[20];
        if (generate_eid(tag.eik, t, candidate) != 0) continue;

        if (memcmp(candidate, seen_eid, 20) == 0) {
            int32_t delta_seconds = delta * (int32_t)ROTATION_PERIOD;
            uint32_t old_offset = g_device_time_offset_seconds;
            g_device_time_offset_seconds = (uint32_t)((int32_t)old_offset - delta_seconds);
            Serial.printf("[AUTO-CORRECT] %s was actually delta=%d away - offset corrected %lu -> %lu "
                          "(drift was ~%ld s)\n",
                          tag.name, delta, (unsigned long)old_offset,
                          (unsigned long)g_device_time_offset_seconds, (long)delta_seconds);
            record_sighting(g_known_stats[worst_idx], real_now, rssi);
            g_cached_address[worst_idx] = address;
            g_cached_address_type[worst_idx] = addressType;
            g_cached_address_known[worst_idx] = true;
            return true;
        }
    }
    return false;
}

// Does the actual crypto comparison - called from loop(), never from the BLE
// callback. Checks every known tag, not just Kyuubi.
static void process_pending_eid(const uint8_t seen_eid[20], int rssi,
                                 const BLEAddress &address, esp_ble_addr_type_t addressType,
                                 uint8_t frame_type, bool has_hashed_flags, uint8_t hashed_flags) {
    // Real wall-clock time, for sighting-gap stats - NOT the same as the
    // device-time-adjusted `now` below, which is only for computing EID
    // candidates against the tag's own unsynced clock.
    uint32_t real_now = current_unix_time();
    uint32_t now = real_now - g_device_time_offset_seconds;

    char flags_str[8];
    if (has_hashed_flags) {
        snprintf(flags_str, sizeof(flags_str), "0x%02x", hashed_flags);
    } else {
        snprintf(flags_str, sizeof(flags_str), "n/a");
    }

    // Check current window plus one on either side, to tolerate clock skew
    // between our manually-entered clock and the tag's own timer - this is
    // also what proves we track across a re-key: as time moves past a
    // rotation boundary, "now" naturally shifts window and we keep matching
    // without needing to re-enter anything.
    for (int tag_idx = 0; tag_idx < NUM_KNOWN_TAGS; tag_idx++) {
        const KnownTag &tag = KNOWN_TAGS[tag_idx];
        for (int delta = -1; delta <= 1; delta++) {
            uint32_t t = now + (uint32_t)(delta * (int)ROTATION_PERIOD);
            uint8_t candidate[20];
            if (generate_eid(tag.eik, t, candidate) != 0) continue;

            if (memcmp(candidate, seen_eid, 20) == 0) {
                // Capture the gap BEFORE record_sighting() updates last_seen_unix.
                char since_str[16];
                if (g_known_stats[tag_idx].ever_seen) {
                    uint32_t gap = (real_now >= g_known_stats[tag_idx].last_seen_unix)
                                       ? (real_now - g_known_stats[tag_idx].last_seen_unix)
                                       : 0;
                    snprintf(since_str, sizeof(since_str), "%lus", (unsigned long)gap);
                } else {
                    snprintf(since_str, sizeof(since_str), "first");
                }

                Serial.printf("[%s] [MATCH] %s resolved! window_offset=%d unix_time~%lu rssi=%d addr=%s "
                              "addrType=%d frameType=0x%02x hashedFlags=%s\n",
                              since_str, tag.name, delta, (unsigned long)t, rssi, address.toString().c_str(),
                              (int)addressType, frame_type, flags_str);
                g_cached_address[tag_idx] = address;
                g_cached_address_type[tag_idx] = addressType;
                g_cached_address_known[tag_idx] = true;
                record_sighting(g_known_stats[tag_idx], real_now, rssi);
                return;
            }
        }
    }

    if (try_widen_and_correct_offset(seen_eid, real_now, rssi, address, addressType)) {
        return; // wasn't actually unknown - just a drifted known tag, now corrected
    }

    record_unknown_sighting(seen_eid, real_now, rssi);

    Serial.printf("[seen non-matching FMDN frame] rssi=%d frameType=0x%02x hashedFlags=%s eid=",
                  rssi, frame_type, flags_str);
    for (int i = 0; i < 20; i++) Serial.printf("%02x", seen_eid[i]);
    Serial.println();
}

// Set by the notify callback in ring_tag() when the Beacon Actions
// characteristic sends an unsolicited notification - per the spec, ringing
// state 0x03 specifically means "stopped because the button was pressed".
// Widened from an initial 4 bytes after seeing real notification data start
// with 0x05 0x0c - looks like [data_id=Ring][data_length=12], which would
// mean the real payload is much longer than the 4-byte "ringing state /
// components / timeout" structure assumed from the spec fetch, and was
// being silently truncated.
static volatile bool g_notify_received = false;
static uint8_t g_notify_data[32];
static volatile size_t g_notify_len = 0;

// Connects to a tag and sends a Ring command over its Beacon Actions
// characteristic, authenticated with the Ring Key derived from its EIK - no
// Google/cloud involvement, this is a direct local BLE GATT interaction.
// Sequence per the Find Hub Network Accessory Specification:
//   1. Connect (plain, unauthenticated at the BLE link layer is fine).
//   2. READ the Beacon Actions char -> [protocol_version(1)][nonce(8)].
//   3. Compute auth_key = first 8 bytes of HMAC-SHA256(ring_key,
//      version || nonce || data_id(0x05) || data_length || additional_data).
//   4. WRITE [data_id(0x05)][data_length][auth_key(8)][additional_data(4)]
//      back to the same characteristic. additional_data is [ring_op(1)]
//      [timeout_deciseconds(2, big-endian?)][volume(1)].
//   5. Subscribe to notifications on the SAME characteristic and keep the
//      connection open for the ring's duration - if the button gets
//      pressed while ringing, the tag notifies [ringing_state(1)]
//      [components(1)][timeout_remaining(2)], where state 0x03 means
//      "stopped due to button press".
// NOTE: the exact meaning of "data_length" (whether it's just the additional
// data's length, or covers more of the payload) wasn't 100% pinned down from
// the spec fetch - first attempt uses length=4 (the additional-data size);
// if the tag doesn't actually ring, this is the first thing to vary.
static bool ring_tag(const BLEAddress &address, esp_ble_addr_type_t addressType, const uint8_t eik[32]) {
    uint8_t ring_key[8];
    derive_ring_key(eik, ring_key);

    Serial.printf("Connecting to %s (addrType=%d) to send Ring command...\n",
                  address.toString().c_str(), (int)addressType);
    BLEClient *pClient = BLEDevice::createClient();
    // Must pass the address type FMDN tags actually advertise with (private/
    // random, not public) - BLEClient::connect() defaults to
    // BLE_ADDR_TYPE_PUBLIC otherwise, which fails immediately.
    if (!pClient->connect(address, addressType)) {
        Serial.println("GATT connect failed.");
        return false;
    }

    BLERemoteService *pService = pClient->getService(FAST_PAIR_SERVICE_UUID);
    if (pService == nullptr) {
        Serial.println("Fast Pair service (0xFE2C) not found on this device.");
        pClient->disconnect();
        return false;
    }

    BLERemoteCharacteristic *pChar = pService->getCharacteristic(BEACON_ACTIONS_CHAR_UUID);
    if (pChar == nullptr) {
        Serial.println("Beacon Actions characteristic not found.");
        pClient->disconnect();
        return false;
    }

    String nonceResp = pChar->readValue();
    if (nonceResp.length() < 9) {
        Serial.printf("Nonce read too short (%d bytes).\n", nonceResp.length());
        pClient->disconnect();
        return false;
    }
    uint8_t protocol_version = (uint8_t)nonceResp[0];
    uint8_t nonce[8];
    memcpy(nonce, nonceResp.c_str() + 1, 8);

    Serial.print("Got protocol_version=");
    Serial.print(protocol_version);
    Serial.print(" nonce=");
    print_hex(nonce, 8);
    Serial.println();

    // additional_data: ring all components (0xFF), 10s timeout (100
    // deciseconds), default volume.
    uint8_t additional_data[4] = {0xFF, 0x00, 0x64, 0x00};
    uint8_t data_id = 0x05;
    uint8_t data_length = sizeof(additional_data);

    uint8_t hmac_msg[1 + 8 + 1 + 1 + sizeof(additional_data)];
    size_t off = 0;
    hmac_msg[off++] = protocol_version;
    memcpy(hmac_msg + off, nonce, 8);
    off += 8;
    hmac_msg[off++] = data_id;
    hmac_msg[off++] = data_length;
    memcpy(hmac_msg + off, additional_data, sizeof(additional_data));
    off += sizeof(additional_data);

    uint8_t auth_key[8];
    hmac_sha256_truncated8(ring_key, sizeof(ring_key), hmac_msg, off, auth_key);

    uint8_t payload[1 + 1 + 8 + sizeof(additional_data)];
    size_t poff = 0;
    payload[poff++] = data_id;
    payload[poff++] = data_length;
    memcpy(payload + poff, auth_key, 8);
    poff += 8;
    memcpy(payload + poff, additional_data, sizeof(additional_data));
    poff += sizeof(additional_data);

    // Subscribe BEFORE writing the ring command, so we don't miss a
    // fast button-press response.
    g_notify_received = false;
    g_notify_len = 0;
    if (pChar->canNotify()) {
        pChar->registerForNotify([](BLERemoteCharacteristic *c, uint8_t *data, size_t length, bool isNotify) {
            (void)c;
            (void)isNotify;
            size_t n = length < sizeof(g_notify_data) ? length : sizeof(g_notify_data);
            memcpy((void *)g_notify_data, data, n);
            g_notify_len = n;
            g_notify_received = true;
        });
    } else {
        Serial.println("Characteristic doesn't support notify - won't catch button-press events.");
    }

    Serial.print("Writing ring command: ");
    print_hex(payload, poff);
    Serial.println();

    pChar->writeValue(payload, poff, true);
    Serial.println("Ring command sent - listen for a beep! Watching for 25s. Do NOT press the "
                    "button until well after it stops on its own, to see a clean timeline.");
    Serial.println("[TICK] markers print every second as a timing reference.");

    // additional_data's timeout was 100 deciseconds (10s) - window is longer
    // than that so there's a clear stretch of silence after the natural stop
    // to press into.
    //
    // Real notifications turned out to be [data_id(1)][data_length(1)]
    // [payload(data_length bytes)], and payload[8]==0x03 is the confirmed
    // button-press signal (see FINDINGS.md section 7) - but a press only
    // generates that standalone signal AFTER the ring has already ended
    // (dismissed or timed out); a press that successfully dismisses an
    // active ring just shows up as the ring's own stop notification, not a
    // separately distinguishable event.
    unsigned long wait_start = millis();
    unsigned long last_tick_s = 0;
    while (millis() - wait_start < 25000UL) {
        unsigned long elapsed = millis() - wait_start;

        unsigned long elapsed_s = elapsed / 1000UL;
        if (elapsed_s != last_tick_s) {
            last_tick_s = elapsed_s;
            Serial.printf("[TICK] t=%lus\n", elapsed_s);
        }

        if (g_notify_received) {
            Serial.printf("[NOTIFY] t=%lu ms  raw: ", elapsed);
            print_hex(g_notify_data, g_notify_len);
            Serial.println();

            if (g_notify_len >= 2) {
                uint8_t data_id = g_notify_data[0];
                uint8_t data_length = g_notify_data[1];
                Serial.printf("  data_id=0x%02x data_length=%d payload=", data_id, data_length);
                size_t payload_len = g_notify_len - 2;
                print_hex(g_notify_data + 2, payload_len);
                Serial.println();

                if (payload_len >= 9) {
                    uint8_t state_byte = g_notify_data[2 + 8];
                    Serial.printf("  payload[8]=0x%02x\n", state_byte);
                    if (state_byte == 0x03) {
                        Serial.println(">>> BUTTON PRESS DETECTED <<<");
                    }
                }
            }
            g_notify_received = false; // keep watching in case of further notifications
        }
        delay(20);
    }

    pClient->disconnect();
    Serial.println("Done watching, disconnected.");
    return true;
}

// Connects and subscribes to the Beacon Actions characteristic's
// notifications WITHOUT ever writing a Ring command - isolates whether
// button presses generate notifications on their own, independent of an
// active ring. Earlier tests always rang first, which left it ambiguous
// whether the payload[8]==0x03 readings were a real press signal or
// somehow tied to the ring being active.
static bool listen_only(const BLEAddress &address, esp_ble_addr_type_t addressType, unsigned long listen_ms) {
    Serial.printf("Connecting to %s (addrType=%d) to LISTEN ONLY (no ring command will be sent)...\n",
                  address.toString().c_str(), (int)addressType);
    BLEClient *pClient = BLEDevice::createClient();
    if (!pClient->connect(address, addressType)) {
        Serial.println("GATT connect failed.");
        return false;
    }

    BLERemoteService *pService = pClient->getService(FAST_PAIR_SERVICE_UUID);
    if (pService == nullptr) {
        Serial.println("Fast Pair service (0xFE2C) not found on this device.");
        pClient->disconnect();
        return false;
    }

    BLERemoteCharacteristic *pChar = pService->getCharacteristic(BEACON_ACTIONS_CHAR_UUID);
    if (pChar == nullptr) {
        Serial.println("Beacon Actions characteristic not found.");
        pClient->disconnect();
        return false;
    }

    g_notify_received = false;
    g_notify_len = 0;
    if (!pChar->canNotify()) {
        Serial.println("Characteristic doesn't support notify - can't listen this way.");
        pClient->disconnect();
        return false;
    }
    pChar->registerForNotify([](BLERemoteCharacteristic *c, uint8_t *data, size_t length, bool isNotify) {
        (void)c;
        (void)isNotify;
        size_t n = length < sizeof(g_notify_data) ? length : sizeof(g_notify_data);
        memcpy((void *)g_notify_data, data, n);
        g_notify_len = n;
        g_notify_received = true;
    });

    Serial.printf("Subscribed, listening for %lu ms - press the button now (no ring will happen)...\n", listen_ms);
    Serial.println("[TICK] markers print every second as a timing reference - line up your press pattern against these.");

    unsigned long wait_start = millis();
    unsigned long last_tick_s = 0;
    while (millis() - wait_start < listen_ms) {
        unsigned long elapsed = millis() - wait_start;

        unsigned long elapsed_s = elapsed / 1000UL;
        if (elapsed_s != last_tick_s) {
            last_tick_s = elapsed_s;
            Serial.printf("[TICK] t=%lus\n", elapsed_s);
        }

        if (g_notify_received) {
            Serial.printf("[NOTIFY] t=%lu ms  raw: ", elapsed);
            print_hex(g_notify_data, g_notify_len);
            Serial.println();
            if (g_notify_len >= 2) {
                uint8_t data_id = g_notify_data[0];
                uint8_t data_length = g_notify_data[1];
                size_t payload_len = g_notify_len - 2;
                Serial.printf("  data_id=0x%02x data_length=%d payload=", data_id, data_length);
                print_hex(g_notify_data + 2, payload_len);
                Serial.println();
                if (payload_len >= 9) {
                    uint8_t state_byte = g_notify_data[2 + 8];
                    Serial.printf("  payload[8]=0x%02x\n", state_byte);
                    if (state_byte == 0x03) {
                        Serial.println(">>> BUTTON PRESS DETECTED <<<");
                    }
                }
            }
            g_notify_received = false;
        }
        delay(20);
    }

    pClient->disconnect();
    Serial.println("Done listening, disconnected.");
    return true;
}

// --- Stats web page ---
static WebServer g_web_server(80);

static void append_stats_row(String &html, const char *name, const SightingStats &stats, uint32_t real_now) {
    html += "<tr><td>";
    html += name;
    html += "</td>";
    if (!stats.ever_seen) {
        html += "<td colspan=6><i>never seen</i></td></tr>";
        return;
    }
    uint32_t seconds_ago = (real_now >= stats.last_seen_unix) ? (real_now - stats.last_seen_unix) : 0;
    html += "<td>" + String(stats.count) + "</td>";
    html += "<td>" + String(seconds_ago) + "s ago</td>";
    html += "<td>" + String(stats.last_rssi) + "</td>";
    if (stats.count > 1) {
        uint32_t gap_count = stats.count - 1;
        uint32_t avg_gap = stats.sum_gap_seconds / gap_count;
        html += "<td>" + String(avg_gap) + "s</td>";
        html += "<td>" + String(stats.min_gap_seconds) + "s</td>";
        html += "<td>" + String(stats.max_gap_seconds) + "s</td>";
    } else {
        html += "<td>n/a</td><td>n/a</td><td>n/a</td>";
    }
    html += "</tr>";
}

static void handle_root() {
    uint32_t real_now = current_unix_time();

    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                   "<meta http-equiv='refresh' content='5'>"
                   "<title>OpenBoat MOB tag stats</title>"
                   "<style>body{font-family:sans-serif} table{border-collapse:collapse} "
                   "td,th{border:1px solid #ccc;padding:4px 8px;text-align:left}</style>"
                   "</head><body>";
    html += "<h2>Known tags</h2>";
    html += "<table><tr><th>Name</th><th>Times seen</th><th>Last seen</th><th>Last RSSI</th>"
             "<th>Avg gap</th><th>Min gap</th><th>Max gap</th></tr>";
    for (int i = 0; i < NUM_KNOWN_TAGS; i++) {
        append_stats_row(html, KNOWN_TAGS[i].name, g_known_stats[i], real_now);
    }
    html += "</table>";

    html += "<h2>Unknown tags (not one of the above)</h2>";
    html += "<table><tr><th>EID (first 6 bytes)</th><th>Times seen</th><th>Last seen</th><th>Last RSSI</th>"
             "<th>Avg gap</th><th>Min gap</th><th>Max gap</th></tr>";
    for (int i = 0; i < UNKNOWN_TABLE_LEN; i++) {
        if (!g_unknown_table[i].used) continue;
        char label[16];
        snprintf(label, sizeof(label), "%02x%02x%02x%02x%02x%02x...",
                 g_unknown_table[i].eid[0], g_unknown_table[i].eid[1], g_unknown_table[i].eid[2],
                 g_unknown_table[i].eid[3], g_unknown_table[i].eid[4], g_unknown_table[i].eid[5]);
        append_stats_row(html, label, g_unknown_table[i].stats, real_now);
    }
    html += "</table>";

    html += "<p><small>Auto-refreshes every 5s. Gaps are real wall-clock seconds between "
            "sightings, not the tag's own (unsynced) device time.</small></p>";
    html += "</body></html>";

    g_web_server.send(200, "text/html", html);
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== EID resolve test, stage 2: live BLE scan ===");

    if (crypto_init() != 0) {
        Serial.println("crypto_init() failed - halting.");
        while (true) delay(1000);
    }

    Serial.print("Self-check against known test vector: ");
    Serial.println(self_check() ? "PASS" : "FAIL");

    // benchmark_generate_eid(); // commented out - slow (~7 min), don't need it every boot

    uint32_t network_time;
    if (try_get_time_from_network(&network_time) == 0) {
        g_base_unix_time = network_time;
        g_base_millis = millis();
        Serial.printf("Clock set from network: unix_time=%lu\n", (unsigned long)g_base_unix_time);
    } else {
        Serial.println("Falling back to manual timestamp entry.");
        prompt_for_time();
    }

    if (WiFi.status() == WL_CONNECTED) {
        g_web_server.on("/", handle_root);
        g_web_server.begin();
        Serial.printf("Stats web page: http://%s/\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("WiFi not connected - stats web page unavailable this run.");
    }

    BLEDevice::init("");
    BLEScan *pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new FmdnAdvertisedDeviceCallbacks(), true);
    pBLEScan->setActiveScan(false);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    Serial.println("Scanning for the MiTag's FMDN broadcast (this repeats indefinitely - "
                    "leave it running to watch it keep resolving across a re-key)...");
    Serial.println("Once a tag has been resolved at least once, type '1'/'2'/'3' + Enter to send "
                    "it a local Ring command (Kyuubi / Nothing Much / Nothing Much 2 - no cloud involved).");
    Serial.println("Type 'x' + Enter to test whether Ring Key auth is actually enforced "
                    "(rings Kyuubi using Nothing Much's key on purpose).");
    Serial.println("Type 'l' + Enter to connect to Kyuubi and just listen for 20s (no ring sent) - "
                    "press the button to see if it notifies on its own.");
}

// Checking Serial.available() only once per loop() iteration meant typing
// 'r' could sit unread for 30-60+ seconds (a 5s scan plus up to ~19s per
// queued non-matching frame, since each checks 3 known tags x 3 windows at
// ~2.1s/call). Call this between every slow step instead of just once at the
// top, so input actually gets noticed promptly.
//
// Type '1', '2', or '3' + Enter to ring that specific known tag (matching
// KNOWN_TAGS order: Kyuubi, Nothing Much, Nothing Much 2) - testing all
// three, not just Kyuubi, tells us whether a connect failure is specific to
// one tag or true of the whole product line (all non-connectable
// advertising, matching the DIY reference firmware we already read).
//
// Type 'x' + Enter to test whether the Ring Key authentication is actually
// enforced: connects to Kyuubi but derives the HMAC from Nothing Much's EIK
// instead of Kyuubi's own - a deliberately WRONG key. If Kyuubi still beeps,
// the tag isn't validating the auth field at all.
//
// Type 'l' + Enter to connect to Kyuubi and just LISTEN for 20s, without
// ever sending a Ring command - isolates whether button presses generate
// notifications on their own, independent of an active ring.
static void check_for_ring_command() {
    if (!Serial.available()) return;
    char c = (char)Serial.read();

    if (c == 'x' || c == 'X') {
        if (g_cached_address_known[0]) {
            Serial.println("Wrong-key test: connecting to Kyuubi, ringing with Nothing Much's key...");
            ring_tag(g_cached_address[0], g_cached_address_type[0], KNOWN_TAGS[1].eik);
        } else {
            Serial.println("Haven't resolved Kyuubi's address yet - wait for a [MATCH] line first.");
        }
        return;
    }

    if (c == 'l' || c == 'L') {
        if (g_cached_address_known[0]) {
            listen_only(g_cached_address[0], g_cached_address_type[0], 30000UL);
        } else {
            Serial.println("Haven't resolved Kyuubi's address yet - wait for a [MATCH] line first.");
        }
        return;
    }

    int idx = -1;
    if (c == '1') idx = 0;
    else if (c == '2') idx = 1;
    else if (c == '3') idx = 2;
    else return;

    if (idx >= NUM_KNOWN_TAGS) {
        Serial.println("No tag at that index.");
        return;
    }

    if (g_cached_address_known[idx]) {
        ring_tag(g_cached_address[idx], g_cached_address_type[idx], KNOWN_TAGS[idx].eik);
    } else {
        Serial.printf("Haven't resolved %s's address yet - wait for a [MATCH] line first.\n", KNOWN_TAGS[idx].name);
    }
}

// Called frequently from loop() (between every slow step, same reasoning as
// check_for_ring_command()'s own comment) so the stats web page doesn't
// become unresponsive during a long scan/crypto cycle.
static void poll_background_tasks() {
    check_for_ring_command();
    g_web_server.handleClient();
}

void loop() {
    poll_background_tasks();

    unsigned long scan_start = millis();
    BLEScan *pBLEScan = BLEDevice::getScan();
    pBLEScan->start(5, false);
    pBLEScan->clearResults();
    unsigned long scan_ms = millis() - scan_start;

    poll_background_tasks();

    // Drain whatever onResult() queued during that scan window, then run the
    // (comparatively slow) crypto comparison here, safely off the BLE stack's
    // own task.
    PendingEid local[PENDING_QUEUE_LEN];
    int count;
    portENTER_CRITICAL(&g_pending_mux);
    count = g_pending_count;
    memcpy(local, g_pending, sizeof(PendingEid) * count);
    g_pending_count = 0;
    portEXIT_CRITICAL(&g_pending_mux);

    unsigned long process_start = millis();
    for (int i = 0; i < count; i++) {
        poll_background_tasks();
        unsigned long frame_start = millis();
        process_pending_eid(local[i].eid, local[i].rssi, local[i].address, local[i].addressType,
                             local[i].frame_type, local[i].has_hashed_flags, local[i].hashed_flags);
        unsigned long frame_ms = millis() - frame_start;
        if (frame_ms > 500) {
            Serial.printf("[TIMING] frame %d/%d took %lums to process\n", i + 1, count, frame_ms);
        }
    }
    unsigned long process_ms = millis() - process_start;

    Serial.printf("[TIMING] cycle: scan=%lums queued=%d process=%lums total=%lums\n",
                  scan_ms, count, process_ms, scan_ms + process_ms);
}
