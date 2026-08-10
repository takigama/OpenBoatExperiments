"""
FMDN EID generation speed test, native Python (not the ESP32).

Ports the same generate_eid() logic already validated on-device (see
ManOverBoard/ESP32/archive_onboard_crypto/display_mob_test_with_onboard_crypto.cpp
and full_featured.cpp) - AES-256-ECB derivation of r', reduced mod the
SECP160r1 curve order, then scalar-multiplied by the generator. Self-checks
against the same known-good test vector the ESP32 code uses before timing
anything, so a passing self-check means this is computing the exact same
thing the firmware does, just on real hardware instead of an ESP32.

Answers: "how many 1024s rotation windows can a real machine search through
in a fixed time budget" - directly relevant to designing the MQTT-based
resolver's cold-start offset discovery (see FINDINGS.md section 4's
benchmark for how brutally slow this was on-device: ~2.1s/call there,
vs whatever this reports here).
"""

import time
from Crypto.Cipher import AES
from ecdsa.ellipticcurve import CurveFp, Point

# SECP160r1 domain parameters, built by hand instead of imported as a named
# curve. Some ecdsa package builds (e.g. the Debian-packaged python3-ecdsa
# on a NanoPi-R4S, vs. a modern pip-installed one) don't re-export
# SECP160r1 at the top level, or register it under a different name. CurveFp
# / Point are low-level primitives that have been stable across ecdsa
# releases for a very long time, so building the curve directly here sidesteps
# whatever a given install decided to name/export from its curve registry.
SECP160R1_P  = int("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFF", 16)
SECP160R1_A  = int("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFC", 16)
SECP160R1_B  = int("1C97BEFC54BD7A8B65ACF89F81D4D4ADC565FA45", 16)
SECP160R1_GX = int("4A96B5688EF573284664698968C38BB913CBFC82", 16)
SECP160R1_GY = int("23A628553168947D59DCC912042351377AC5FB32", 16)
SECP160R1_N  = int("0100000000000000000001F4C8F927AED3CA752257", 16)

_curve = CurveFp(SECP160R1_P, SECP160R1_A, SECP160R1_B)
SECP160R1_GENERATOR = Point(_curve, SECP160R1_GX, SECP160R1_GY, SECP160R1_N)

K = 10
ROTATION_PERIOD = 1 << K  # 1024

# Same known-good test vector as the ESP32 firmware's self_check().
TEST_KEY = bytes(range(32))  # 0x00..0x1f
TEST_TIMESTAMP = 1700000000
EXPECTED_EID_HEX = "6f3bcc7d38665e6cadf7ca48e9ce6d3ea3942d83"


def build_r_dash(identity_key: bytes, masked_timestamp: int) -> bytes:
    ts_bytes = masked_timestamp.to_bytes(4, "big")
    data = bytes([0xFF] * 11) + bytes([K]) + ts_bytes + bytes([0x00] * 11) + bytes([K]) + ts_bytes
    cipher = AES.new(identity_key, AES.MODE_ECB)
    return cipher.encrypt(data)


def generate_eid(identity_key: bytes, timestamp: int) -> bytes:
    masked_timestamp = timestamp & ~(ROTATION_PERIOD - 1)
    r_dash = build_r_dash(identity_key, masked_timestamp)
    r = int.from_bytes(r_dash, "big") % SECP160R1_N
    point = r * SECP160R1_GENERATOR
    x = point.x()
    return x.to_bytes(20, "big")


def self_check() -> bool:
    eid = generate_eid(TEST_KEY, TEST_TIMESTAMP)
    return eid.hex() == EXPECTED_EID_HEX


def main():
    print("Self-check against known test vector:", "PASS" if self_check() else "FAIL")
    if not self_check():
        print("Aborting - crypto port doesn't match the validated ESP32 firmware, results would be meaningless.")
        return

    # Real tag EIK not needed for a pure speed test - any 32-byte key
    # produces the same per-call cost. Using a fixed dummy key, not one of
    # the real extracted EIKs (no need to touch secrets.h for this).
    dummy_key = bytes(range(32))

    duration_secs = 120
    print(f"\nRunning generate_eid() as fast as possible for {duration_secs}s...")

    t = 0  # arbitrary starting window - each iteration steps forward one rotation period
    count = 0
    start = time.perf_counter()
    deadline = start + duration_secs
    while time.perf_counter() < deadline:
        generate_eid(dummy_key, t)
        t += ROTATION_PERIOD
        count += 1
    elapsed = time.perf_counter() - start

    calls_per_sec = count / elapsed
    us_per_call = (elapsed / count) * 1_000_000
    total_span_secs = count * ROTATION_PERIOD

    print(f"\n{count} calls in {elapsed:.2f}s -> {calls_per_sec:.1f} calls/sec ({us_per_call:.1f} us/call)")
    print(f"Total drift range coverable in {duration_secs}s of searching:")
    print(f"  {total_span_secs} rotation-windows-worth of seconds")
    print(f"  = {total_span_secs / 3600:.1f} hours")
    print(f"  = {total_span_secs / 86400:.1f} days")
    print(f"  = {total_span_secs / 86400 / 365.25:.2f} years")

    # Compare directly against the ESP32's own benchmarked cost (~2.118ms...
    # actually ~2.1s/call, i.e. ~2,118,727us/call per FINDINGS.md section 4).
    esp32_us_per_call = 2_118_727
    speedup = esp32_us_per_call / us_per_call
    print(f"\nFor reference, the ESP32 benchmark (FINDINGS.md section 4) measured "
          f"~{esp32_us_per_call/1e6:.2f}s/call - this machine is ~{speedup:.0f}x faster.")


if __name__ == "__main__":
    main()
