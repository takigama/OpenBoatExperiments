"""
FMDN EID generation - the same generate_eid() logic already validated
on-device (see ../../ESP32/archive_onboard_crypto/display_mob_test_with_onboard_crypto.cpp)
and cross-platform (../../ESP32/drift_test/drift_test.py) - AES-256-ECB
derivation of r', reduced mod the SECP160r1 curve order, then
scalar-multiplied by the generator. self_check() must pass before trusting
anything computed here; a passing check means this is the exact same math
the firmware/drift_test.py already validated, not a fresh reimplementation.
"""

from ecdsa.ellipticcurve import CurveFp, Point
from Crypto.Cipher import AES

# SECP160r1 domain parameters, built by hand instead of imported as a named
# curve - see drift_test.py's comment for why (some ecdsa package builds
# don't re-export SECP160r1 at the top level or under a consistent name;
# CurveFp/Point are low-level primitives stable across ecdsa releases).
SECP160R1_P = int("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFF", 16)
SECP160R1_A = int("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFC", 16)
SECP160R1_B = int("1C97BEFC54BD7A8B65ACF89F81D4D4ADC565FA45", 16)
SECP160R1_GX = int("4A96B5688EF573284664698968C38BB913CBFC82", 16)
SECP160R1_GY = int("23A628553168947D59DCC912042351377AC5FB32", 16)
SECP160R1_N = int("0100000000000000000001F4C8F927AED3CA752257", 16)

_curve = CurveFp(SECP160R1_P, SECP160R1_A, SECP160R1_B)
SECP160R1_GENERATOR = Point(_curve, SECP160R1_GX, SECP160R1_GY, SECP160R1_N)

K = 10
ROTATION_PERIOD = 1 << K  # 1024 seconds (~17 minutes) per FMDN rotation window

# Same known-good test vector the ESP32 firmware's self_check() and
# drift_test.py use - a passing check here confirms this port is computing
# the exact same thing, not a subtly-different reimplementation.
TEST_KEY = bytes(range(32))
TEST_TIMESTAMP = 1700000000
EXPECTED_EID_HEX = "6f3bcc7d38665e6cadf7ca48e9ce6d3ea3942d83"


def mask_timestamp(timestamp: int) -> int:
    """Rounds down to the start of the current ROTATION_PERIOD window."""
    return timestamp & ~(ROTATION_PERIOD - 1)


def build_r_dash(identity_key: bytes, masked_timestamp: int) -> bytes:
    ts_bytes = masked_timestamp.to_bytes(4, "big")
    data = bytes([0xFF] * 11) + bytes([K]) + ts_bytes + bytes([0x00] * 11) + bytes([K]) + ts_bytes
    cipher = AES.new(identity_key, AES.MODE_ECB)
    return cipher.encrypt(data)


def generate_eid(identity_key: bytes, timestamp: int) -> bytes:
    """identity_key is the tag's 32-byte EIK. Returns the 20-byte EID for
    whichever rotation window `timestamp` falls in - stateless per window,
    not a chain: generating for a timestamp 3 windows ago costs exactly the
    same as generating for right now (see FINDINGS.md for why that matters
    for a resolver precomputing a rolling window of candidates)."""
    masked_timestamp = mask_timestamp(timestamp)
    r_dash = build_r_dash(identity_key, masked_timestamp)
    r = int.from_bytes(r_dash, "big") % SECP160R1_N
    point = r * SECP160R1_GENERATOR
    x = point.x()
    return x.to_bytes(20, "big")


def self_check() -> bool:
    return generate_eid(TEST_KEY, TEST_TIMESTAMP).hex() == EXPECTED_EID_HEX


if __name__ == "__main__":
    print("Self-check:", "PASS" if self_check() else "FAIL")
