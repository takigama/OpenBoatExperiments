#include "rx_log.h"

#include <math.h>
#include <stdarg.h>

namespace RxLog {

namespace {

struct Ring {
    Entry e[kPerSource];
    size_t head = 0;      // next slot to write
    uint32_t total = 0;
};

Ring s_rings[kSourceCount];
uint32_t s_seq = 0;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

constexpr double kMsToKnots = 1.0 / 0.514444;

}  // namespace

const char *sourceName(Source s) {
    switch (s) {
        case Source::SeaTalk: return "SeaTalk";
        case Source::Can: return "CAN";
        case Source::Mqtt: return "MQTT";
        case Source::SignalK: return "SignalK";
    }
    return "?";
}

void add(Source s, const char *fmt, ...) {
    char tmp[kTextMax];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);

    uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    Ring &r = s_rings[(int)s];
    Entry &e = r.e[r.head];
    e.seq = ++s_seq;
    e.ms = now;
    e.src = s;
    memcpy(e.text, tmp, sizeof(e.text));
    r.head = (r.head + 1) % kPerSource;
    r.total++;
    portEXIT_CRITICAL(&s_mux);
}

void addEvent(Source s, const SeatalkDecode::Event &ev, const char *note) {
    char d[kTextMax];
    describe(ev, d, sizeof(d));
    if (note && note[0]) add(s, "%s  [%s]", d, note);
    else add(s, "%s", d);
}

void hex(const uint8_t *data, size_t len, size_t maxBytes, char *out, size_t cap) {
    if (!cap) return;
    size_t n = len < maxBytes ? len : maxBytes;
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < cap; i++) {
        o += snprintf(out + o, cap - o, "%s%02X", i ? " " : "", data[i]);
    }
    if (n < len && o + 4 < cap) o += snprintf(out + o, cap - o, "...");
    out[o < cap ? o : cap - 1] = 0;
}

void describe(const SeatalkDecode::Event &ev, char *out, size_t cap) {
    using T = SeatalkDecode::Type;
    const double deg = 180.0 / M_PI;
    switch (ev.type) {
        case T::Depth:
            snprintf(out, cap, "Depth %.1f m", ev.value);
            break;
        case T::SpeedThroughWater:
            snprintf(out, cap, "Speed through water %.1f kn", ev.value * kMsToKnots);
            break;
        case T::TripLog:
            snprintf(out, cap, "Trip log %.2f nm", ev.value / 1852.0);
            break;
        case T::TotalLog:
            snprintf(out, cap, "Total log %.1f nm", ev.value / 1852.0);
            break;
        case T::ApparentWindAngle:
            snprintf(out, cap, "Apparent wind angle %.0f deg", ev.value * deg);
            break;
        case T::ApparentWindSpeed:
            snprintf(out, cap, "Apparent wind speed %.1f kn", ev.value * kMsToKnots);
            break;
        case T::WaterTemperature:
            snprintf(out, cap, "Water temp %.1f C", ev.value - 273.15);
            break;
        case T::Latitude:
            snprintf(out, cap, "Latitude %.5f", ev.value);
            break;
        case T::Longitude:
            snprintf(out, cap, "Longitude %.5f", ev.value);
            break;
        case T::SpeedOverGround:
            snprintf(out, cap, "Speed over ground %.1f kn", ev.value * kMsToKnots);
            break;
        case T::CourseOverGround:
            snprintf(out, cap, "Course over ground %.0f deg", ev.value * deg);
            break;
        case T::GnssTime: {
            int s = (int)ev.value;
            snprintf(out, cap, "GNSS time %02d:%02d:%02d UTC", s / 3600, (s / 60) % 60, s % 60);
            break;
        }
        case T::GnssDate:
            snprintf(out, cap, "GNSS date %04d-%02d-%02d", ev.year, ev.month, ev.day);
            break;
        case T::SatelliteCount:
            snprintf(out, cap, "Satellites %d", (int)ev.value);
            break;
        case T::HeadingAndRudder:
            snprintf(out, cap, "Heading %.0f deg, rudder %.0f deg", ev.value * deg, ev.value2 * deg);
            break;
        case T::MagneticVariation:
            snprintf(out, cap, "Magnetic variation %.1f deg", ev.value * deg);
            break;
        default:
            snprintf(out, cap, "type %d = %.3f", (int)ev.type, ev.value);
            break;
    }
}

bool findOlder(uint8_t srcMask, uint32_t beforeSeq, Entry *out) {
    bool found = false;
    uint32_t bestSeq = 0;
    portENTER_CRITICAL(&s_mux);
    for (int s = 0; s < kSourceCount; s++) {
        if (!(srcMask & (1 << s))) continue;
        const Ring &r = s_rings[s];
        for (size_t i = 0; i < kPerSource; i++) {
            const Entry &e = r.e[i];
            if (e.seq && e.seq < beforeSeq && e.seq > bestSeq) {
                bestSeq = e.seq;
                *out = e;
                found = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

uint32_t total(Source s) {
    portENTER_CRITICAL(&s_mux);
    uint32_t t = s_rings[(int)s].total;
    portEXIT_CRITICAL(&s_mux);
    return t;
}

}  // namespace RxLog
