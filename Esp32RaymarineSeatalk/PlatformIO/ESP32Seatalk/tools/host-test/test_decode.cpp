// host test: SeaTalk decode / encode against datagrams captured from a real bus
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "seatalk_decode.h"
#include "seatalk_encode.h"

static std::vector<std::vector<uint8_t>> g_sent;
namespace SeatalkBus {
void send(uint8_t command, const uint8_t *data, uint8_t len) {
    std::vector<uint8_t> v{command};
    v.insert(v.end(), data, data + len);
    g_sent.push_back(v);
}
}  // namespace SeatalkBus

static SeatalkDecode::Event dec(std::initializer_list<uint8_t> bytes, bool *ok = nullptr) {
    SeatalkBus::Datagram dg{};
    int n = 0;
    for (uint8_t b : bytes) dg.bytes[n++] = b;
    dg.length = n;
    SeatalkDecode::Event ev;
    bool r = SeatalkDecode::decode(dg, &ev);
    if (ok) *ok = r;
    return ev;
}
static double deg(const SeatalkDecode::Event &e) { return e.value * 180.0 / M_PI; }
static bool near(double a, double b, double t = 0.051) { return fabs(a - b) < t; }

int main() {
    using T = SeatalkDecode::Type;
    bool ok;

    // --- wind angle, from the real log: 142, 143, 141, 140 degrees
    struct { uint8_t y; double want; } rows[] = {{0x1c, 142.0}, {0x1e, 143.0}, {0x1a, 141.0}, {0x18, 140.0}};
    for (auto &r : rows) {
        auto e = dec({0x10, 0x11, 0x01, r.y}, &ok);
        printf("10 11 01 %02x -> %.1f deg\n", r.y, deg(e));
        assert(ok && e.type == T::ApparentWindAngle && near(deg(e), r.want));
    }
    // 45 deg starboard; 279 deg (= 81 deg to port, negative); exactly 180
    assert(near(deg(dec({0x10, 0x01, 0x00, 0x5A})), 45.0));
    assert(near(deg(dec({0x10, 0x01, 0x02, 0x2E})), -81.0));
    assert(near(deg(dec({0x10, 0x01, 0x01, 0x68})), 180.0));

    // --- encode -> exactly the bytes the real instrument sends, and back
    g_sent.clear();
    SeatalkEncode::sendApparentWindAngle(142.0);
    assert(g_sent.size() == 1 && g_sent[0] == (std::vector<uint8_t>{0x10, 0x01, 0x01, 0x1C}));
    for (double a : {0.0, 45.0, 90.0, 142.0, 179.5, 180.0, 270.0, 359.5, -81.0, -0.5, -179.0, 725.0}) {
        g_sent.clear();
        SeatalkEncode::sendApparentWindAngle(a);
        auto &d = g_sent[0];
        SeatalkBus::Datagram dg{};
        for (size_t i = 0; i < d.size(); i++) dg.bytes[i] = d[i];
        dg.length = d.size();
        SeatalkDecode::Event e;
        assert(SeatalkDecode::decode(dg, &e));
        double want = fmod(a, 360.0); if (want > 180.0) want -= 360.0; if (want < -180.0) want += 360.0;
        printf("encode %7.1f -> %02X %02X %02X -> decodes to %6.1f (expect %6.1f)\n", a, d[1], d[2], d[3], deg(e), want);
        assert(near(deg(e), want, 0.26) || near(fabs(deg(e)), 180.0, 0.26));
    }

    // --- the "no sensor" placeholder from the log
    auto t = dec({0x23, 0x41, 0x28, 0x68}, &ok);
    printf("23 41 28 68 -> %.2f C, flagA=%d, isNoSensor=%d\n", t.value - 273.15, t.flagA, SeatalkDecode::isNoSensor(t));
    assert(ok && near(t.value - 273.15, 40.0) && t.flagA && SeatalkDecode::isNoSensor(t));
    auto t2 = dec({0x23, 0x01, 0x12, 0x35});
    assert(!SeatalkDecode::isNoSensor(t2) && near(t2.value - 273.15, 18.0));

    // --- everything else in the log still decodes as before
    assert(near(dec({0x00, 0x42, 0x60, 0x42, 0x01}).value, 9.815, 0.001));   // depth, LE
    assert(near(dec({0x11, 0x11, 0x09, 0x07}).value, 4.990, 0.001));         // wind speed 9.7 kn
    assert(near(dec({0x11, 0x11, 0x08, 0x09}).value, 4.579, 0.001));         // 8.9 kn
    bool u; dec({0x24, 0x42, 0x00, 0x00, 0x00}, &u); assert(!u);             // display units: not decoded
    dec({0xA3, 0x90, 0x11}, &u); assert(!u);
    printf("PASS\n");
}
