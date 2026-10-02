package pages

import (
	"fmt"
	"math"
	"strings"
	"time"

	"signalkpaperdisplay/internal/ais"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// BoxKind is one thing a Nav box can show.
type BoxKind struct {
	ID    string // stored in settings.json; never change one
	Name  string // as listed in the settings picker
	Label string // as drawn above the value in the box
}

// BoxAIS is the one kind that isn't a single number: it fills its box with
// the nearest AIS contacts.
const BoxAIS = "ais3"

// BoxKinds is everything a Nav box can show, in the order the settings picker
// lists it. Add a row here and a case in boxMetric and it appears there.
var BoxKinds = []BoxKind{
	{"sog", "Speed over ground", "SOG"},
	{"stw", "Speed through water", "STW"},
	{"cog", "Course over ground", "COG"},
	{"hdg", "Heading", "HEADING"},
	{"vmgw", "VMG to wind", "VMG WIND"},
	{"vmgwp", "VMG to waypoint", "VMG WPT"},
	{"depth", "Depth", "DEPTH"},
	{"wtemp", "Water temperature", "WATER"},
	{"aws", "Apparent wind speed", "WIND SPEED"},
	{"awa", "Apparent wind angle", "WIND ANGLE"},
	{"tws", "True wind speed", "TRUE WIND"},
	{"twd", "True wind direction", "TRUE WIND DIR"},
	{"wpbrg", "Waypoint bearing", "WPT BEARING"},
	{"wpdist", "Waypoint distance", "WPT DIST"},
	{"wpttg", "Time to waypoint", "WPT TIME"},
	{"wpttw", "Time to WP at speed", "WPT TIME@SPD"},
	{BoxAIS, "3 closest AIS", "CLOSEST AIS"},

	{"cpa", "Closest approach", "CPA"},
	{"tcpa", "Time to closest", "TCPA"},
	{"hdgm", "Magnetic heading", "MAG HEADING"},
	{"rot", "Rate of turn", "TURN RATE"},
	{"pitch", "Pitch", "PITCH"},
	{"roll", "Roll", "ROLL"},
	{"rudder", "Rudder angle", "RUDDER"},
	{"ap", "Autopilot mode", "AUTOPILOT"},
	{"aptgt", "Autopilot target", "AP TARGET"},
	{"xte", "Cross-track error", "XTE"},
	{"eta", "Arrival time", "ETA"},
	{"airtemp", "Air temperature", "AIR"},
	{"baro", "Air pressure", "PRESSURE"},
	{"humidity", "Humidity", "HUMIDITY"},
	{"batv", "Battery volts", "BATTERY"},
	{"batsoc", "Battery charge", "BATT CHARGE"},
	{"bati", "Battery current", "BATT AMPS"},
	{"rpm", "Engine RPM", "RPM"},
	{"engtemp", "Engine temperature", "ENGINE TEMP"},
	{"oilp", "Oil pressure", "OIL PRESS"},
	{"fuelrate", "Fuel use", "FUEL RATE"},
	{"fresh", "Fresh water", "FRESH WATER"},
	{"waste", "Grey water", "GREY WATER"},
	{"black", "Black water", "BLACK WATER"},
}

// NavBoxes is how many boxes the Nav page has.
const NavBoxes = navCols * navRows

// DefaultBoxes is the Nav page's layout until the user changes it.
func DefaultBoxes() []string {
	return []string{"sog", "hdg", "depth", "cog", "vmgw", BoxAIS}
}

// BoxKindByID finds a kind by its stored ID.
func BoxKindByID(id string) (BoxKind, bool) {
	for _, k := range BoxKinds {
		if k.ID == id {
			return k, true
		}
	}
	return BoxKind{}, false
}

// NormalizeBoxes returns exactly NavBoxes valid kind IDs: entries that are
// missing or not a known kind take the default for their position. A copy is
// returned, so callers may keep or change it freely.
func NormalizeBoxes(ids []string) []string {
	out := DefaultBoxes()
	for i := range out {
		if i < len(ids) {
			if _, ok := BoxKindByID(ids[i]); ok {
				out[i] = ids[i]
			}
		}
	}
	return out
}

// wpVMG is the speed we're making towards the next waypoint, m/s: the
// server's figure if it sends one, else worked out from our course and speed
// over the ground. Negative means we're moving away from it.
func wpVMG(own signalk.Own, now time.Time) (float64, bool) {
	if own.WPVMG.Fresh(now, StaleAfter) {
		return own.WPVMG.V, true
	}
	if !own.WPBearing.Fresh(now, StaleAfter) || !own.COG.Fresh(now, StaleAfter) || !own.SOG.Fresh(now, StaleAfter) {
		return 0, false
	}
	return own.SOG.V * math.Cos(own.COG.V-own.WPBearing.V), true
}

// wpTimeAtSpeed is how long the next waypoint is away at the speed we're
// currently making towards it, in seconds. Unavailable when we're not closing
// on it (or barely moving), where the answer would be infinite or negative.
func wpTimeAtSpeed(own signalk.Own, now time.Time) (float64, bool) {
	if !own.WPDistance.Fresh(now, StaleAfter) {
		return 0, false
	}
	v, ok := wpVMG(own, now)
	if !ok || v < 0.1 { // 0.1 m/s is about 0.2 knots
		return 0, false
	}
	return own.WPDistance.V / v, true
}

// formatDuration shows seconds as "m:ss" under an hour and "h:mm" above, with
// a unit saying which. Beyond 99 hours there's no point being precise.
func formatDuration(sec float64) (value, unit string) {
	switch {
	case sec < 0 || math.IsNaN(sec):
		return "--", ""
	case sec < 3600:
		s := int(sec + 0.5)
		return fmt.Sprintf("%d:%02d", s/60, s%60), "m:s"
	case sec < 100*3600:
		m := int(sec/60 + 0.5)
		return fmt.Sprintf("%d:%02d", m/60, m%60), "h:m"
	default:
		return ">99", "h"
	}
}

// BoxesPerPicker is how many kinds fit on one page of the settings picker.
const BoxesPerPicker = 18

// BoxPageOf is the picker page a kind is on.
func BoxPageOf(id string) int {
	for i, k := range BoxKinds {
		if k.ID == id {
			return i / BoxesPerPicker
		}
	}
	return 0
}

// BoxPages is how many picker pages there are.
func BoxPages() int { return (len(BoxKinds) + BoxesPerPicker - 1) / BoxesPerPicker }

// short is a name cut to n characters, for a label with little room.
func short(name string, n int) string {
	r := []rune(strings.ToUpper(strings.TrimSpace(name)))
	if len(r) > n {
		r = r[:n]
	}
	return string(r)
}

// deg formats an angle in radians as degrees with the given decimals.
func deg(rad float64, decimals int) string {
	return fmt.Sprintf("%.*f", decimals, rad*180/math.Pi)
}

// sideAngle formats an angle in radians, either side of the bow, as degrees
// with "stbd" or "port" as the unit.
func sideAngle(a float64, decimals int) (value, unit string) {
	a = math.Mod(a, 2*math.Pi)
	if a > math.Pi {
		a -= 2 * math.Pi
	} else if a < -math.Pi {
		a += 2 * math.Pi
	}
	unit = "stbd"
	if a < 0 {
		unit = "port"
	}
	return deg(math.Abs(a), decimals), unit
}

// percent formats a 0..1 ratio as a whole percentage.
func percent(ratio float64) string { return fmt.Sprintf("%.0f", ratio*100) }

// boxMetric works out the label, unit and value text for a single-number box.
// ok is false when the data isn't there or has gone stale, which the box
// shows as dashes. contacts are the AIS contacts, nearest first.
func boxMetric(id string, own signalk.Own, now time.Time, e Env, contacts []ais.Contact) metric {
	k, _ := BoxKindByID(id)
	m := metric{label: k.Label}
	fresh := func(r signalk.Reading) bool { return r.Fresh(now, StaleAfter) }
	bearing := func(rad float64) string { return fmt.Sprintf("%03.0f", degrees(rad)) }

	switch id {
	case "sog":
		m.value, m.unit = e.Units.Format("sog", own.SOG.V)
		m.ok = fresh(own.SOG)
	case "stw":
		m.value, m.unit = e.Units.Format("stw", own.STW.V)
		m.ok = fresh(own.STW)
	case "cog":
		m.value, m.ok = bearing(own.COG.V), cogLive(own, now) // meaningless while stopped
	case "hdg":
		m.value, m.ok = bearing(own.Heading.V), fresh(own.Heading)
	case "vmgw":
		v, ok := vmg(own, now)
		m.value, m.unit = e.Units.Format("sog", v)
		m.ok = ok
	case "vmgwp":
		v, ok := wpVMG(own, now)
		m.value, m.unit = e.Units.Format("sog", v)
		m.ok = ok
	case "depth":
		m.value, m.unit = e.Units.Format("depth", own.Depth.V)
		m.ok = fresh(own.Depth)
	case "wtemp":
		m.value, m.unit = e.Units.Format("watertemp", own.WaterTemp.V)
		m.ok = own.WaterTemp.Fresh(now, SlowStaleAfter)
	case "aws":
		m.value, m.unit = e.Units.Format("aws", own.AWS.V)
		m.ok = fresh(own.AWS)
	case "awa":
		// Relative to the bow, 0..180 on whichever side it's on.
		a := math.Mod(own.AWA.V, 2*math.Pi)
		if a > math.Pi {
			a -= 2 * math.Pi
		} else if a < -math.Pi {
			a += 2 * math.Pi
		}
		m.value = fmt.Sprintf("%.0f", math.Abs(a)*180/math.Pi)
		m.unit = "stbd"
		if a < 0 {
			m.unit = "port"
		}
		m.ok = fresh(own.AWA)
	case "tws":
		m.value, m.unit = e.Units.Format("tws", own.TWS.V)
		m.ok = fresh(own.TWS)
	case "twd":
		m.value, m.ok = bearing(own.TWD.V), fresh(own.TWD)
	case "wpbrg":
		m.value, m.ok = bearing(own.WPBearing.V), fresh(own.WPBearing)
	case "wpdist":
		m.value, m.unit = e.Units.Format("range", own.WPDistance.V)
		m.ok = fresh(own.WPDistance)
	case "wpttg":
		m.value, m.unit = formatDuration(own.WPTimeToGo.V)
		m.ok = fresh(own.WPTimeToGo)
	case "wpttw":
		sec, ok := wpTimeAtSpeed(own, now)
		m.value, m.unit = formatDuration(sec)
		m.ok = ok
	default:
		boxExtra(&m, id, own, now, e, contacts)
	}
	return m
}

// boxExtra fills in the kinds that come from the extra SignalK paths or from
// the AIS contacts, which are all alike in being only sometimes there.
func boxExtra(m *metric, id string, own signalk.Own, now time.Time, e Env, contacts []ais.Contact) {
	fresh := func(r signalk.Reading) bool { return r.Fresh(now, StaleAfter) }
	slow := func(r signalk.Reading) bool { return r.Fresh(now, SlowStaleAfter) }
	get := own.ExtraReading
	first := own.FirstExtra

	switch id {
	case "cpa", "tcpa":
		// The contact that will pass us closest, of those getting nearer.
		c, ok := ais.MostUrgent(contacts)
		m.ok = ok
		if ok {
			m.label += " " + short(c.DisplayName(), 9)
			if id == "cpa" {
				m.value, m.unit = e.Units.Format("range", c.CPA)
			} else {
				m.value, m.unit = formatDuration(c.TCPA)
			}
		}
	case "hdgm":
		r := get("navigation.headingMagnetic")
		m.value, m.ok = fmt.Sprintf("%03.0f", degrees(r.V)), fresh(r)
	case "rot":
		r := get("navigation.rateOfTurn")
		m.value, m.unit, m.ok = fmt.Sprintf("%+.0f", r.V*180/math.Pi*60), "°/min", fresh(r)
	case "pitch":
		r := get("navigation.attitude.pitch")
		m.value, m.unit, m.ok = fmt.Sprintf("%+.1f", r.V*180/math.Pi), "°", fresh(r)
	case "roll":
		r := get("navigation.attitude.roll")
		m.value, m.unit, m.ok = fmt.Sprintf("%+.1f", r.V*180/math.Pi), "°", fresh(r)
	case "rudder":
		r := get("steering.rudderAngle")
		m.value, m.unit = sideAngle(r.V, 1) // positive is to starboard
		m.ok = fresh(r)
	case "ap":
		m.value, m.ok = strings.ToUpper(own.Autopilot.S), own.Autopilot.Fresh(now, SlowStaleAfter)
	case "aptgt":
		r := get("steering.autopilot.target.headingTrue")
		m.value, m.ok = fmt.Sprintf("%03.0f", degrees(r.V)), slow(r)
	case "xte":
		// Cross-track error: how far off the line to the waypoint. Positive
		// means the boat is to starboard of it, which the label says.
		r := get("navigation.course.calcValues.crossTrackError")
		if !r.Valid() {
			r = get("navigation.courseGreatCircle.crossTrackError")
		}
		if !r.Valid() {
			r = get("navigation.courseRhumbline.crossTrackError")
		}
		m.value, m.unit = e.Units.Format("range", math.Abs(r.V))
		if r.V < 0 {
			m.label = "XTE PORT"
		} else {
			m.label = "XTE STBD"
		}
		m.ok = fresh(r)
	case "eta":
		// When we arrive, from the server's time to go or else from the
		// speed we're making towards it, as a local clock time.
		sec, ok := 0.0, false
		if own.WPTimeToGo.Fresh(now, StaleAfter) {
			sec, ok = own.WPTimeToGo.V, true
		} else {
			sec, ok = wpTimeAtSpeed(own, now)
		}
		if ok && sec >= 0 {
			at := now.Add(time.Duration(sec * float64(time.Second)))
			m.value = at.Format("15:04")
			if days := int(at.Sub(time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, now.Location())).Hours() / 24); days > 0 {
				m.unit = fmt.Sprintf("+%dd", days)
			}
			m.ok = true
		}
	case "airtemp":
		r := get("environment.outside.temperature")
		m.value, m.unit = e.Units.Format("watertemp", r.V)
		m.ok = slow(r)
	case "baro":
		r := get("environment.outside.pressure")
		m.value, m.unit = e.Units.FormatQuantity(units.Pressure, r.V)
		m.ok = slow(r)
	case "humidity":
		r := get("environment.outside.humidity")
		if !r.Valid() {
			r = get("environment.outside.relativeHumidity")
		}
		m.value, m.unit, m.ok = percent(r.V), "%", slow(r)
	case "batv":
		r := first("electrical.batteries.", ".voltage")
		m.value, m.unit, m.ok = fmt.Sprintf("%.1f", r.V), "V", slow(r)
	case "batsoc":
		r := first("electrical.batteries.", ".capacity.stateOfCharge")
		m.value, m.unit, m.ok = percent(r.V), "%", slow(r)
	case "bati":
		r := first("electrical.batteries.", ".current")
		m.value, m.unit, m.ok = fmt.Sprintf("%+.1f", r.V), "A", slow(r)
	case "rpm":
		r := first("propulsion.", ".revolutions")
		m.value, m.unit, m.ok = fmt.Sprintf("%.0f", r.V*60), "rpm", fresh(r) // SignalK gives revolutions per second
	case "engtemp":
		r := first("propulsion.", ".temperature")
		m.value, m.unit = e.Units.Format("watertemp", r.V)
		m.ok = slow(r)
	case "oilp":
		// Pascals: bar, or psi under the imperial preset.
		r := first("propulsion.", ".oilPressure")
		if e.Units.Preset == units.PresetImperial {
			m.value, m.unit = fmt.Sprintf("%.0f", r.V*0.000145038), "psi"
		} else {
			m.value, m.unit = fmt.Sprintf("%.1f", r.V*0.00001), "bar"
		}
		m.ok = slow(r)
	case "fuelrate":
		// Cubic metres per second: litres an hour, or US gallons an hour.
		r := first("propulsion.", ".fuel.rate")
		if e.Units.Preset == units.PresetImperial {
			m.value, m.unit = fmt.Sprintf("%.1f", r.V*3.6e6*0.264172), "gal/h"
		} else {
			m.value, m.unit = fmt.Sprintf("%.1f", r.V*3.6e6), "L/h"
		}
		m.ok = slow(r)
	case "fresh":
		r := first("tanks.freshWater.", ".currentLevel")
		m.value, m.unit, m.ok = percent(r.V), "%", slow(r)
	case "waste":
		r := first("tanks.wasteWater.", ".currentLevel")
		m.value, m.unit, m.ok = percent(r.V), "%", slow(r)
	case "black":
		r := first("tanks.blackWater.", ".currentLevel")
		m.value, m.unit, m.ok = percent(r.V), "%", slow(r)
	}
}
