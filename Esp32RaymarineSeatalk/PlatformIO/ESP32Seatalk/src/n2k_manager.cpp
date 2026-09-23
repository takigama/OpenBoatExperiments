#include "n2k_manager.h"

#include <N2kMessages.h>
#include <NMEA2000.h>

#include "debug_log.h"
#include "n2k_twai_driver.h"
#include "route_config.h"

namespace N2kManager {

namespace {

constexpr gpio_num_t kCanTxPin = GPIO_NUM_7;   // CAN_TX_LL, header J4 pin 3 - see hardware project
constexpr gpio_num_t kCanRxPin = GPIO_NUM_10;  // CAN_RX_LL, header J4 pin 4

tNMEA2000_Twai s_n2k(kCanTxPin, kCanRxPin);
bool s_open = false;

// TX-side caches: SeaTalk delivers these as independent datagrams, but
// their N2K PGNs want them bundled - same combining pattern
// SignalKManager uses for position, applied here to position again plus
// heading+variation (folded into the Heading PGN - see header comment).
double s_txLat = 0, s_txLon = 0;
bool s_txHasLat = false, s_txHasLon = false;
double s_txVariation = 0;
bool s_txHasVariation = false;

// RX-side cache: N2K keeps heading (PGN 127250) and rudder (PGN 127245)
// as two separate messages, but SeatalkDecode::Type::HeadingAndRudder -
// and therefore MqttManager/SignalKManager's handling of it - expects
// both together in one Event, so they're combined the same way before
// being relayed onward.
double s_rxHeading = 0, s_rxRudder = 0;
bool s_rxHasHeading = false, s_rxHasRudder = false;

// Routes one already-decoded event through RouteConfig::relay(), which
// decides the rest: MQTT/SignalK always (fixed leg), plus SeaTalk TX if
// the user's enabled Can->SeaTalk for this object type - never back onto
// CAN itself (would just echo a device's own PGN back at it).
void relay(const SeatalkDecode::Event &ev) {
    DebugLog::logf("n2k: rx type=%d value=%.3f value2=%.3f", (int)ev.type, ev.value, ev.value2);
    RouteConfig::relay(RouteConfig::Bus::Can, ev);
}

void relaySimple(SeatalkDecode::Type type, double value) {
    SeatalkDecode::Event ev;
    ev.type = type;
    ev.value = value;
    relay(ev);
}

void handleN2kMsg(const tN2kMsg &N2kMsg) {
    unsigned char sid;
    switch (N2kMsg.PGN) {
        case 128259UL: {  // Speed (water referenced)
            double stw, swrt;
            tN2kSpeedWaterReferenceType swrtType;
            if (ParseN2kBoatSpeed(N2kMsg, sid, stw, swrt, swrtType) && stw != N2kDoubleNA) {
                relaySimple(SeatalkDecode::Type::SpeedThroughWater, stw);
            }
            break;
        }
        case 128267UL: {  // Water depth
            double depth, offset, range;
            if (ParseN2kWaterDepth(N2kMsg, sid, depth, offset, range) && depth != N2kDoubleNA) {
                relaySimple(SeatalkDecode::Type::Depth, depth);
            }
            break;
        }
        case 130306UL: {  // Wind data
            double windSpeed, windAngle;
            tN2kWindReference ref;
            if (ParseN2kWindSpeed(N2kMsg, sid, windSpeed, windAngle, ref)) {
                if (windSpeed != N2kDoubleNA) relaySimple(SeatalkDecode::Type::ApparentWindSpeed, windSpeed);
                if (windAngle != N2kDoubleNA) relaySimple(SeatalkDecode::Type::ApparentWindAngle, windAngle);
            }
            break;
        }
        case 130310UL: {  // Environmental parameters (water temp)
            double waterTemp, airTemp, pressure;
            if (ParseN2kPGN130310(N2kMsg, sid, waterTemp, airTemp, pressure) && waterTemp != N2kDoubleNA) {
                relaySimple(SeatalkDecode::Type::WaterTemperature, waterTemp);
            }
            break;
        }
        case 129025UL: {  // Position, rapid update
            double lat, lon;
            if (ParseN2kPGN129025(N2kMsg, lat, lon)) {
                if (lat != N2kDoubleNA) relaySimple(SeatalkDecode::Type::Latitude, lat);
                if (lon != N2kDoubleNA) relaySimple(SeatalkDecode::Type::Longitude, lon);
            }
            break;
        }
        case 129026UL: {  // COG/SOG, rapid update
            tN2kHeadingReference ref;
            double cog, sog;
            if (ParseN2kCOGSOGRapid(N2kMsg, sid, ref, cog, sog)) {
                if (cog != N2kDoubleNA) relaySimple(SeatalkDecode::Type::CourseOverGround, cog);
                if (sog != N2kDoubleNA) relaySimple(SeatalkDecode::Type::SpeedOverGround, sog);
            }
            break;
        }
        case 127250UL: {  // Vessel heading
            double heading, deviation, variation;
            tN2kHeadingReference ref;
            if (ParseN2kHeading(N2kMsg, sid, heading, deviation, variation, ref) && heading != N2kDoubleNA) {
                s_rxHeading = heading;
                s_rxHasHeading = true;
                if (variation != N2kDoubleNA) relaySimple(SeatalkDecode::Type::MagneticVariation, variation);
                if (s_rxHasRudder) {
                    SeatalkDecode::Event ev;
                    ev.type = SeatalkDecode::Type::HeadingAndRudder;
                    ev.value = s_rxHeading;
                    ev.value2 = s_rxRudder;
                    relay(ev);
                }
            }
            break;
        }
        case 127245UL: {  // Rudder
            double rudder;
            unsigned char instance;
            tN2kRudderDirectionOrder dirOrder;
            double angleOrder;
            if (ParseN2kRudder(N2kMsg, rudder, instance, dirOrder, angleOrder) && rudder != N2kDoubleNA) {
                s_rxRudder = rudder;
                s_rxHasRudder = true;
                if (s_rxHasHeading) {
                    SeatalkDecode::Event ev;
                    ev.type = SeatalkDecode::Type::HeadingAndRudder;
                    ev.value = s_rxHeading;
                    ev.value2 = s_rxRudder;
                    relay(ev);
                }
            }
            break;
        }
        default:
            break;  // PGN we don't map yet - not logged individually, would drown out everything else
    }
}

// Every PGN shares the same "SID=0 (no sequence grouping), everything
// else default/not-available" shape here - this board has no other N2K
// messages to group these with via a shared sequence ID, so 0 throughout
// is correct, not a placeholder standing in for something unimplemented.
void sendPosition(double lat, double lon) {
    tN2kMsg msg;
    SetN2kLatLonRapid(msg, lat, lon);
    s_n2k.SendMsg(msg);
}

void sendHeading(double headingRad) {
    tN2kMsg msg;
    double variation = s_txHasVariation ? s_txVariation : N2kDoubleNA;
    SetN2kPGN127250(msg, 0, headingRad, N2kDoubleNA, variation, N2khr_magnetic);
    s_n2k.SendMsg(msg);
}

}  // namespace

void begin() {
    s_n2k.SetProductInformation("esp32seatalk-1",                  // model serial code
                                 100,                                // product code
                                 "ESP32Seatalk SeaTalk/N2K Bridge",  // model ID
                                 "1.0.0",                            // sw version
                                 "1.0.0");                           // model version
    // Function 130 (PC Gateway) / Class 25 (Inter/Intranetwork Device),
    // manufacturer code 2046 - the standard placeholder this library's
    // own examples use for hobbyist devices with no registered NMEA
    // manufacturer code.
    s_n2k.SetDeviceInformation((uint32_t)ESP.getEfuseMac() & 0x1FFFFF, 130, 25, 2046);
    s_n2k.SetMode(tNMEA2000::N2km_ListenAndNode, 60);
    s_n2k.SetMsgHandler(handleN2kMsg);
    s_n2k.Open();
    s_open = true;
    DebugLog::logf("n2k: CAN bus opened on TX=GPIO%d RX=GPIO%d (unverified - no N2K bus available to test against)",
                    (int)kCanTxPin, (int)kCanRxPin);
}

void tick() { s_n2k.ParseMessages(); }

bool isOpen() { return s_open; }

void publishDecoded(const SeatalkDecode::Event &ev) {
    switch (ev.type) {
        case SeatalkDecode::Type::Depth: {
            tN2kMsg msg;
            SetN2kWaterDepth(msg, 0, ev.value, N2kDoubleNA);
            s_n2k.SendMsg(msg);
            return;
        }
        case SeatalkDecode::Type::SpeedThroughWater: {
            tN2kMsg msg;
            SetN2kBoatSpeed(msg, 0, ev.value);
            s_n2k.SendMsg(msg);
            return;
        }
        case SeatalkDecode::Type::ApparentWindAngle:
        case SeatalkDecode::Type::ApparentWindSpeed: {
            // Both halves of one N2K wind PGN, but SeaTalk sends them as
            // separate datagrams too - unlike position/heading, there's
            // no strong reason to wait for both here, so each just goes
            // out immediately with whichever half we actually have,
            // matching the two-datagram cadence they already arrive at.
            tN2kMsg msg;
            if (ev.type == SeatalkDecode::Type::ApparentWindAngle) {
                SetN2kWindSpeed(msg, 0, N2kDoubleNA, ev.value, N2kWind_Apparent);
            } else {
                SetN2kWindSpeed(msg, 0, ev.value, N2kDoubleNA, N2kWind_Apparent);
            }
            s_n2k.SendMsg(msg);
            return;
        }
        case SeatalkDecode::Type::WaterTemperature: {
            tN2kMsg msg;
            SetN2kPGN130310(msg, 0, ev.value);
            s_n2k.SendMsg(msg);
            return;
        }
        case SeatalkDecode::Type::Latitude:
            s_txLat = ev.value;
            s_txHasLat = true;
            if (s_txHasLon) sendPosition(s_txLat, s_txLon);
            return;
        case SeatalkDecode::Type::Longitude:
            s_txLon = ev.value;
            s_txHasLon = true;
            if (s_txHasLat) sendPosition(s_txLat, s_txLon);
            return;
        case SeatalkDecode::Type::SpeedOverGround:
        case SeatalkDecode::Type::CourseOverGround: {
            // Same "send each half immediately" reasoning as wind above -
            // COG/SOG rapid update (129026) takes both, but SeaTalk gives
            // them to us separately (commands 0x52/0x53).
            tN2kMsg msg;
            if (ev.type == SeatalkDecode::Type::CourseOverGround) {
                SetN2kCOGSOGRapid(msg, 0, N2khr_true, ev.value, N2kDoubleNA);
            } else {
                SetN2kCOGSOGRapid(msg, 0, N2khr_true, N2kDoubleNA, ev.value);
            }
            s_n2k.SendMsg(msg);
            return;
        }
        case SeatalkDecode::Type::HeadingAndRudder: {
            sendHeading(ev.value);
            tN2kMsg msg;
            SetN2kRudder(msg, ev.value2);
            s_n2k.SendMsg(msg);
            return;
        }
        case SeatalkDecode::Type::MagneticVariation:
            // Folded into the next Heading PGN rather than sent as its
            // own PGN 127258 - see header comment.
            s_txVariation = ev.value;
            s_txHasVariation = true;
            return;
        default:
            return;  // TripLog, TotalLog, GnssTime, GnssDate, SatelliteCount - deliberately not mapped yet
    }
}

}  // namespace N2kManager
