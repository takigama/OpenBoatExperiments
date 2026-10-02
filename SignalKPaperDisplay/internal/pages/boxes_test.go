package pages

import (
	"math"
	"strings"
	"testing"
	"time"

	"signalkpaperdisplay/internal/ais"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func rd(v float64) signalk.Reading { return signalk.Reading{V: v, At: compassNow} }

func metricEnv() Env { return Env{Units: units.Settings{Preset: units.PresetMetric}} }

// everything is a boat with every kind of data present and fresh.
func everything() (signalk.Own, []ais.Contact) {
	own := signalk.Own{
		Heading: rd(0.1), COG: rd(0.2), SOG: rd(3), STW: rd(3), Depth: rd(10), WaterTemp: rd(290),
		AWA: rd(0.5), AWS: rd(5), TWD: rd(1), TWS: rd(6),
		WPBearing: rd(0.3), WPDistance: rd(5000), WPTimeToGo: rd(1800),
		Autopilot: signalk.TextReading{S: "auto", At: compassNow},
		Extra: map[string]signalk.Reading{
			"navigation.headingMagnetic":                        rd(0.2),
			"navigation.rateOfTurn":                             rd(0.001),
			"navigation.attitude.pitch":                         rd(0.02),
			"navigation.attitude.roll":                          rd(-0.1),
			"steering.rudderAngle":                              rd(-0.05),
			"steering.autopilot.target.headingTrue":             rd(0.4),
			"navigation.course.calcValues.crossTrackError":      rd(-30),
			"environment.outside.temperature":                   rd(293),
			"environment.outside.pressure":                      rd(101300),
			"environment.outside.humidity":                      rd(0.55),
			"electrical.batteries.house.voltage":                rd(12.6),
			"electrical.batteries.house.capacity.stateOfCharge": rd(0.87),
			"electrical.batteries.house.current":                rd(-5),
			"propulsion.main.revolutions":                       rd(25),
			"propulsion.main.temperature":                       rd(355),
			"propulsion.main.oilPressure":                       rd(300000),
			"propulsion.main.fuel.rate":                         rd(0.000002),
			"tanks.freshWater.0.currentLevel":                   rd(0.6),
			"tanks.wasteWater.0.currentLevel":                   rd(0.2),
			"tanks.blackWater.0.currentLevel":                   rd(0.1),
		},
	}
	contacts := []ais.Contact{{ID: "urn:mrn:imo:mmsi:235000002", Name: "Aura", Converging: true, CPA: 300, TCPA: 240, Range: 2000}}
	return own, contacts
}

func TestBoxKindsAreWellFormed(t *testing.T) {
	seen := map[string]bool{}
	for _, k := range BoxKinds {
		if k.ID == "" || k.Name == "" || k.Label == "" {
			t.Errorf("incomplete kind %+v", k)
		}
		if seen[k.ID] {
			t.Errorf("duplicate kind ID %q", k.ID)
		}
		seen[k.ID] = true
	}
	// Every kind but the AIS list must be computable by boxMetric: a kind
	// added to the list and forgotten there would draw a blank box.
	own, contacts := everything()
	for _, k := range BoxKinds {
		if k.ID == BoxAIS {
			continue
		}
		m := boxMetric(k.ID, own, compassNow, metricEnv(), contacts)
		// Some labels grow (CPA names the ship, XTE says which side).
		if !m.ok || m.value == "" || !strings.HasPrefix(m.label, strings.Fields(k.Label)[0]) {
			t.Errorf("%s: boxMetric = %+v, want a live value labelled %q", k.ID, m, k.Label)
		}
	}
	// With none of the data, every kind is "not ok" (shown as dashes): a box
	// never invents a value.
	for _, k := range BoxKinds {
		if k.ID == BoxAIS {
			continue
		}
		if m := boxMetric(k.ID, signalk.Own{}, compassNow, metricEnv(), nil); m.ok {
			t.Errorf("%s with no data should not be ok: %+v", k.ID, m)
		}
	}
	if len(DefaultBoxes()) != NavBoxes {
		t.Errorf("defaults have %d boxes, want %d", len(DefaultBoxes()), NavBoxes)
	}
}

func TestExtraKindFormatting(t *testing.T) {
	own, contacts := everything()
	imperial := Env{Units: units.Settings{Preset: units.PresetImperial}}
	cases := []struct {
		id, value, unit string
		env             Env
	}{
		{"hdgm", "011", "", metricEnv()},
		{"rot", "+3", "°/min", metricEnv()}, // 0.001 rad/s = 3.4 deg/min
		{"pitch", "+1.1", "°", metricEnv()},
		{"roll", "-5.7", "°", metricEnv()},
		{"rudder", "2.9", "port", metricEnv()},
		{"ap", "AUTO", "", metricEnv()},
		{"aptgt", "023", "", metricEnv()},
		{"airtemp", "19.9", "°C", metricEnv()},
		{"airtemp", "67.7", "°F", imperial},
		{"baro", "1013", "hPa", metricEnv()},
		{"baro", "29.91", "inHg", imperial},
		{"humidity", "55", "%", metricEnv()},
		{"batv", "12.6", "V", metricEnv()},
		{"batsoc", "87", "%", metricEnv()},
		{"bati", "-5.0", "A", metricEnv()},
		{"rpm", "1500", "rpm", metricEnv()}, // 25 revolutions a second
		{"engtemp", "81.9", "°C", metricEnv()},
		{"oilp", "3.0", "bar", metricEnv()},
		{"oilp", "44", "psi", imperial},
		{"fuelrate", "7.2", "L/h", metricEnv()},
		{"fuelrate", "1.9", "gal/h", imperial},
		{"fresh", "60", "%", metricEnv()},
		{"waste", "20", "%", metricEnv()},
		{"black", "10", "%", metricEnv()},
		{"xte", "0.03", "km", metricEnv()},
		{"cpa", "0.30", "km", metricEnv()},
		{"tcpa", "4:00", "m:s", metricEnv()},
	}
	for _, c := range cases {
		m := boxMetric(c.id, own, compassNow, c.env, contacts)
		if m.value != c.value || m.unit != c.unit {
			t.Errorf("%s = %q %q, want %q %q", c.id, m.value, m.unit, c.value, c.unit)
		}
	}
}

func TestCPABoxesNameTheShipAndPickTheUrgentOne(t *testing.T) {
	own, _ := everything()
	contacts := []ais.Contact{
		{ID: "a", Name: "Far Away", Converging: true, CPA: 2000, TCPA: 100},
		{ID: "b", Name: "To Keel a Sunset", Converging: true, CPA: 150, TCPA: 600},
		{ID: "c", Name: "Leaving", Converging: false},
	}
	m := boxMetric("cpa", own, compassNow, metricEnv(), contacts)
	if m.label != "CPA TO KEEL A" || m.value != "0.15" {
		t.Errorf("cpa = %+v, want the closest-passing ship named (cut to fit) and its distance", m)
	}
	if m := boxMetric("tcpa", own, compassNow, metricEnv(), contacts); m.value != "10:00" {
		t.Errorf("tcpa = %+v, want that same ship's time, 10:00", m)
	}
	// Nothing converging: dashes, not a made-up number.
	if m := boxMetric("cpa", own, compassNow, metricEnv(), []ais.Contact{{ID: "c", Converging: false}}); m.ok {
		t.Error("with nothing converging the CPA box must show dashes")
	}
}

func TestXTESaysWhichSide(t *testing.T) {
	own, _ := everything()
	if m := boxMetric("xte", own, compassNow, metricEnv(), nil); m.label != "XTE PORT" {
		t.Errorf("a negative error is to port: %+v", m)
	}
	own.Extra["navigation.course.calcValues.crossTrackError"] = rd(30)
	if m := boxMetric("xte", own, compassNow, metricEnv(), nil); m.label != "XTE STBD" {
		t.Errorf("a positive error is to starboard: %+v", m)
	}
	// The older course paths work too.
	delete(own.Extra, "navigation.course.calcValues.crossTrackError")
	own.Extra["navigation.courseGreatCircle.crossTrackError"] = rd(-60)
	if m := boxMetric("xte", own, compassNow, metricEnv(), nil); !m.ok || m.value != "0.06" {
		t.Errorf("great-circle xte = %+v", m)
	}
}

func TestETAIsALocalClockTime(t *testing.T) {
	now := time.Date(2026, 10, 3, 10, 0, 0, 0, time.UTC)
	own := signalk.Own{WPTimeToGo: signalk.Reading{V: 5400, At: now}} // 90 minutes
	if m := boxMetric("eta", own, now, metricEnv(), nil); m.value != "11:30" || m.unit != "" || !m.ok {
		t.Errorf("eta = %+v, want 11:30", m)
	}
	// Tomorrow: the day count shows.
	own.WPTimeToGo = signalk.Reading{V: 20 * 3600, At: now}
	if m := boxMetric("eta", own, now, metricEnv(), nil); m.value != "06:00" || m.unit != "+1d" {
		t.Errorf("eta tomorrow = %+v, want 06:00 +1d", m)
	}
	// No server time to go: worked out from the speed made towards it.
	own = signalk.Own{COG: signalk.Reading{V: 1, At: now}, SOG: signalk.Reading{V: 5, At: now},
		WPBearing: signalk.Reading{V: 1, At: now}, WPDistance: signalk.Reading{V: 18000, At: now}} // 3600 s
	if m := boxMetric("eta", own, now, metricEnv(), nil); m.value != "11:00" || !m.ok {
		t.Errorf("eta from speed = %+v, want 11:00", m)
	}
	if m := boxMetric("eta", signalk.Own{}, now, metricEnv(), nil); m.ok {
		t.Error("with no waypoint there is no ETA")
	}
}

func TestSlowValuesStayLongerThanFastOnes(t *testing.T) {
	own, _ := everything()
	old := compassNow.Add(-30 * time.Second) // stale for a 5 s value, fine for a slow one
	for k, r := range own.Extra {
		r.At = old
		own.Extra[k] = r
	}
	for _, id := range []string{"rudder", "rpm", "pitch", "hdgm"} {
		if m := boxMetric(id, own, compassNow, metricEnv(), nil); m.ok {
			t.Errorf("%s is a fast value and 30 s old is stale", id)
		}
	}
	for _, id := range []string{"airtemp", "baro", "batv", "fresh", "engtemp"} {
		if m := boxMetric(id, own, compassNow, metricEnv(), nil); !m.ok {
			t.Errorf("%s changes slowly, so 30 s old is still fine", id)
		}
	}
}

func TestNormalizeBoxes(t *testing.T) {
	def := DefaultBoxes()
	if got := NormalizeBoxes(nil); strings.Join(got, ",") != strings.Join(def, ",") {
		t.Errorf("nil -> %v, want defaults", got)
	}
	got := NormalizeBoxes([]string{"stw", "nonsense", "wpdist"})
	want := append([]string{"stw", def[1], "wpdist"}, def[3:]...)
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Errorf("got %v, want %v (unknown entries and missing tail take defaults)", got, want)
	}
	long := NormalizeBoxes([]string{"sog", "sog", "sog", "sog", "sog", "sog", "sog", "sog", "sog", "sog", "sog", "sog"})
	if len(long) != NavBoxes {
		t.Errorf("extra entries must be dropped, got %d", len(long))
	}
	// The result is a copy: changing it must not change the defaults.
	got[0] = "changed"
	if DefaultBoxes()[0] == "changed" {
		t.Error("NormalizeBoxes shares storage with the defaults")
	}
}

func TestFormatDuration(t *testing.T) {
	cases := []struct {
		sec         float64
		value, unit string
	}{
		{0, "0:00", "m:s"}, {59, "0:59", "m:s"}, {125, "2:05", "m:s"}, {3599, "59:59", "m:s"},
		{3600, "1:00", "h:m"}, {5400, "1:30", "h:m"}, {36000, "10:00", "h:m"}, {99 * 3600, "99:00", "h:m"},
		{101 * 3600, ">99", "h"}, {-5, "--", ""},
	}
	for _, c := range cases {
		if v, u := formatDuration(c.sec); v != c.value || u != c.unit {
			t.Errorf("formatDuration(%v) = %q %q, want %q %q", c.sec, v, u, c.value, c.unit)
		}
	}
}

func TestWaypointSpeedAndTime(t *testing.T) {
	// Heading straight for the waypoint at 4 m/s, 2000 m away: 500 s.
	own := signalk.Own{COG: rd(1), SOG: rd(4), WPBearing: rd(1), WPDistance: rd(2000)}
	if v, ok := wpVMG(own, compassNow); !ok || math.Abs(v-4) > 1e-9 {
		t.Errorf("wpVMG = %v %v, want 4", v, ok)
	}
	if s, ok := wpTimeAtSpeed(own, compassNow); !ok || math.Abs(s-500) > 1e-6 {
		t.Errorf("time at speed = %v %v, want 500", s, ok)
	}
	// 60 degrees off the line: only half the speed counts towards it.
	own.COG = rd(1 + math.Pi/3)
	if v, _ := wpVMG(own, compassNow); math.Abs(v-2) > 1e-9 {
		t.Errorf("wpVMG off-line = %v, want 2", v)
	}
	if s, _ := wpTimeAtSpeed(own, compassNow); math.Abs(s-1000) > 1e-6 {
		t.Errorf("time off-line = %v, want 1000", s)
	}
	// The server's own figure wins when it sends one.
	own.WPVMG = rd(1)
	if v, _ := wpVMG(own, compassNow); v != 1 {
		t.Errorf("server VMG ignored: %v", v)
	}
	// Moving away (or not at all): no time to arrival.
	own.WPVMG = signalk.Reading{}
	own.COG = rd(1 + math.Pi)
	if _, ok := wpTimeAtSpeed(own, compassNow); ok {
		t.Error("moving away from the waypoint must not give an arrival time")
	}
	own.COG, own.SOG = rd(1), rd(0.02)
	if _, ok := wpTimeAtSpeed(own, compassNow); ok {
		t.Error("barely moving must not give an arrival time")
	}
	// Stale inputs.
	own.SOG = rd(4)
	own.WPDistance.At = compassNow.Add(-time.Minute)
	if _, ok := wpTimeAtSpeed(own, compassNow); ok {
		t.Error("a stale distance must not give an arrival time")
	}
}

func TestBoxMetricFormatting(t *testing.T) {
	own := signalk.Own{
		Heading: rd(0), AWA: rd(-0.7854), WPDistance: rd(3704), WPTimeToGo: rd(5400), TWD: rd(math.Pi),
	}
	if m := boxMetric("awa", own, compassNow, metricEnv(), nil); m.value != "45" || m.unit != "port" {
		t.Errorf("wind angle to port: %+v", m)
	}
	own.AWA = rd(0.7854)
	if m := boxMetric("awa", own, compassNow, metricEnv(), nil); m.value != "45" || m.unit != "stbd" {
		t.Errorf("wind angle to starboard: %+v", m)
	}
	// Angles are folded to the short way round: 350 degrees is 10 to port.
	own.AWA = rd(350 * math.Pi / 180)
	if m := boxMetric("awa", own, compassNow, metricEnv(), nil); m.value != "10" || m.unit != "port" {
		t.Errorf("wind angle 350: %+v", m)
	}
	if m := boxMetric("wpdist", own, compassNow, metricEnv(), nil); m.value != "3.70" || m.unit != "km" {
		t.Errorf("distance in metric: %+v", m)
	}
	nautical := Env{Units: units.Settings{Preset: units.PresetNautical}}
	if m := boxMetric("wpdist", own, compassNow, nautical, nil); m.value != "2.00" || m.unit != "nm" {
		t.Errorf("distance in nautical: %+v", m)
	}
	if m := boxMetric("wpttg", own, compassNow, metricEnv(), nil); m.value != "1:30" || m.unit != "h:m" {
		t.Errorf("time to go: %+v", m)
	}
	if m := boxMetric("twd", own, compassNow, metricEnv(), nil); m.value != "180" {
		t.Errorf("true wind direction: %+v", m)
	}
	// Data that never arrived, or has gone stale, is not ok.
	for _, id := range []string{"wpbrg", "wpttw", "vmgwp", "stw", "wtemp"} {
		if m := boxMetric(id, own, compassNow, metricEnv(), nil); m.ok {
			t.Errorf("%s with no data should not be ok: %+v", id, m)
		}
	}
	own.WPDistance.At = compassNow.Add(-time.Minute)
	if m := boxMetric("wpdist", own, compassNow, metricEnv(), nil); m.ok {
		t.Error("a stale waypoint distance must show dashes")
	}
}

func TestOldSavedAISKindIsStillUnderstood(t *testing.T) {
	// "ais3" was the three-closest list in settings files from before it became
	// the closest one: it must keep its box, not fall back to a default.
	got := NormalizeBoxes([]string{"stw", "ais3", "depth"})
	if got[1] != BoxAIS {
		t.Errorf("a saved ais3 became %q, want %q", got[1], BoxAIS)
	}
	if _, ok := BoxKindByID("ais3"); ok {
		t.Error("ais3 is no longer a kind of its own")
	}
	// A file saved with six boxes gets the new default for the two it lacks.
	def := DefaultBoxes()
	six := NormalizeBoxes([]string{"sog", "hdg", "depth", "cog", "vmgw", "ais3"})
	if len(six) != NavBoxes || six[6] != def[6] || six[7] != def[7] {
		t.Errorf("six saved boxes became %v", six)
	}
}
