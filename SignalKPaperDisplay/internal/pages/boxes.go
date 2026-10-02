package pages

import (
	"fmt"
	"math"
	"time"

	"signalkpaperdisplay/internal/signalk"
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

// boxMetric works out the label, unit and value text for a single-number box.
// ok is false when the data isn't there or has gone stale, which the box
// shows as dashes.
func boxMetric(id string, own signalk.Own, now time.Time, e Env) metric {
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
	}
	return m
}
