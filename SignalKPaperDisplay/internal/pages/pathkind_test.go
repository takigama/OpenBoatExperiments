package pages

import (
	"image"
	"math"
	"testing"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

var (
	metricUnits   = units.Settings{Preset: units.PresetMetric}
	imperialUnits = units.Settings{Preset: units.PresetImperial}
	nauticalUnits = units.Settings{Preset: units.PresetNautical}
)

func TestFormatPathFromNames(t *testing.T) {
	deg := func(d float64) float64 { return d * math.Pi / 180 }
	for _, tc := range []struct {
		path        string
		v           float64
		u           units.Settings
		value, unit string
	}{
		{"navigation.speedOverGround", 3, metricUnits, "10.8", "km/h"},
		{"navigation.speedThroughWater", 3, nauticalUnits, "5.8", "kn"},
		{"environment.wind.speedTrue", 5, metricUnits, "18.0", "km/h"},
		{"environment.depth.belowTransducer", 10, metricUnits, "10.0", "m"},
		{"environment.depth.belowKeel", 10, imperialUnits, "32.8", "ft"},
		{"navigation.courseOverGroundTrue", deg(49), metricUnits, "049", "°"},
		{"navigation.headingMagnetic", deg(-10), metricUnits, "350", "°"},
		{"environment.wind.angleApparent", deg(-22), metricUnits, "22", "port"},
		{"environment.wind.angleTrueWater", deg(30), metricUnits, "30", "stbd"},
		{"navigation.attitude.roll", deg(3.8), metricUnits, "+3.8", "°"},
		{"navigation.attitude.pitch", deg(-1), metricUnits, "-1.0", "°"},
		{"navigation.rateOfTurn", deg(1) / 60, metricUnits, "+1", "°/min"},
		{"environment.water.temperature", 293.15, metricUnits, "20.0", "°C"},
		{"environment.outside.temperature", 293.15, imperialUnits, "68.0", "°F"},
		{"environment.outside.pressure", 101300, metricUnits, "1013", "hPa"},
		{"environment.outside.pressure", 101300, imperialUnits, "29.91", "inHg"},
		{"environment.outside.humidity", 0.62, metricUnits, "62", "%"},
		{"tanks.fuel.0.currentLevel", 0.82, metricUnits, "82", "%"},
		{"electrical.batteries.house.capacity.stateOfCharge", 0.9, metricUnits, "90", "%"},
		{"electrical.batteries.house.voltage", 12.69, metricUnits, "12.7", "V"},
		{"electrical.batteries.house.current", -5.65, metricUnits, "-5.7", "A"},
		{"propulsion.main.revolutions", 28.59, metricUnits, "1715", "rpm"},
		{"propulsion.main.oilPressure", 302000, metricUnits, "3.0", "bar"},
		{"propulsion.main.oilPressure", 302000, imperialUnits, "44", "psi"},
		{"propulsion.main.fuel.rate", 0.0000014, metricUnits, "5.0", "L/h"},
		{"navigation.course.calcValues.timeToGo", 754, metricUnits, "12:34", "m:s"},
		{"navigation.course.calcValues.distance", 5556, nauticalUnits, "3.00", "nm"},
		{"navigation.course.calcValues.crossTrackError", 40, nauticalUnits, "0.02", "nm"},
		{"some.unknown.thing", 12.3456, metricUnits, "12.3", ""},
		{"some.unknown.big", 123456, metricUnits, "123456", ""},
		{"some.unknown.small", 0.0123, metricUnits, "0.012", ""},
		{"some.unknown.zero", 0, metricUnits, "0", ""},
	} {
		value, unit := FormatPath(tc.path, "", tc.v, tc.u)
		if value != tc.value || unit != tc.unit {
			t.Errorf("%s = %v: got %q %q, want %q %q", tc.path, tc.v, value, unit, tc.value, tc.unit)
		}
	}
}

func TestFormatPathPrefersWhatTheServerSays(t *testing.T) {
	for _, tc := range []struct {
		path, units string
		v           float64
		u           units.Settings
		value, unit string
	}{
		// A name the dashboard cannot read, but the server says what it is.
		{"vendor.custom.thing", "m/s", 3, metricUnits, "10.8", "km/h"},
		{"vendor.custom.thing", "m/s", 3, nauticalUnits, "5.8", "kn"},
		{"vendor.custom.thing", "K", 300.15, metricUnits, "27.0", "°C"},
		{"vendor.custom.thing", "Pa", 100000, metricUnits, "1000", "hPa"},
		{"vendor.custom.thing", "ratio", 0.5, metricUnits, "50", "%"},
		{"vendor.custom.thing", "V", 13.8, metricUnits, "13.8", "V"},
		{"vendor.custom.thing", "Hz", 50, metricUnits, "50.0", "Hz"},
		{"vendor.custom.thing", "W", 1200, metricUnits, "1200", "W"},
		{"vendor.custom.thing", "s", 90, metricUnits, "1:30", "m:s"},
		{"vendor.custom.thing", "rad", math.Pi / 2, metricUnits, "+90.0", "°"},
		{"vendor.custom.thing", "m", 12, metricUnits, "12.0", "m"},
		// Units that need the name too: metres are a depth or a distance, a radian a
		// direction or an angle off the bow.
		{"navigation.something.distance", "m", 1852, nauticalUnits, "1.00", "nm"},
		{"navigation.headingTrue", "rad", math.Pi, metricUnits, "180", "°"},
		{"environment.wind.angleApparent", "rad", math.Pi / 2, metricUnits, "90", "stbd"},
		// Units it does not know fall back to the name.
		{"environment.water.temperature", "furlongs", 293.15, metricUnits, "20.0", "°C"},
	} {
		value, unit := FormatPath(tc.path, tc.units, tc.v, tc.u)
		if value != tc.value || unit != tc.unit {
			t.Errorf("%s in %q = %v: got %q %q, want %q %q", tc.path, tc.units, tc.v, value, unit, tc.value, tc.unit)
		}
	}
}

func TestPathLabels(t *testing.T) {
	for path, want := range map[string]string{
		"environment.outside.pressure":                      "OUTSIDE PRESSURE",
		"environment.wind.speedApparent":                    "WIND SPEED APPARENT",
		"environment.depth.belowTransducer":                 "DEPTH BELOW TRANSDUCER",
		"navigation.rateOfTurn":                             "RATE OF TURN",
		"navigation.speedThroughWater":                      "SPEED THROUGH WATER",
		"navigation.attitude.roll":                          "ATTITUDE ROLL",
		"propulsion.main.oilPressure":                       "MAIN OIL PRESSURE",
		"tanks.fuel.0.currentLevel":                         "FUEL 0 CURRENT LEVEL",
		"electrical.batteries.house.capacity.stateOfCharge": "HOUSE CAPACITY STATE OF CHARGE",
		"vendor.thing":                                      "VENDOR THING",
		"single":                                            "SINGLE",
	} {
		if got := PathLabel(path); got != want {
			t.Errorf("PathLabel(%q) = %q, want %q", path, got, want)
		}
	}
}

func TestPathKindIDs(t *testing.T) {
	id := PathKindID("environment.outside.pressure")
	if !IsPathKind(id) || PathOf(id) != "environment.outside.pressure" || !ValidPathKind(id) {
		t.Errorf("%q is not a good path kind", id)
	}
	for _, bad := range []string{"path:", "path:a..b", "path:.a", "path:a.", "path:a b", "path:a;b", "path:a/b", "sog", ""} {
		if ValidPathKind(bad) {
			t.Errorf("%q should not be a valid path kind", bad)
		}
	}
	if k, ok := BoxKindByID(id); !ok || k.Name != "environment.outside.pressure" || k.Label != "OUTSIDE PRESSURE" {
		t.Errorf("BoxKindByID(%q) = %+v %v", id, k, ok)
	}
	if _, ok := BoxKindByID("path:a b"); ok {
		t.Error("a malformed path must not become a kind")
	}
	if got := WatchedPaths("sog", id, id, "path:x.y", "depth"); len(got) != 2 || got[0] != "environment.outside.pressure" || got[1] != "x.y" {
		t.Errorf("WatchedPaths = %v", got)
	}
}

func TestBoxesKeepPathKinds(t *testing.T) {
	id := PathKindID("environment.outside.pressure")
	got := NormalizeBoxes([]string{id, "nope", "path:bad path", "depth"})
	if got[0] != id {
		t.Errorf("a path kind was dropped: %v", got)
	}
	if got[1] != DefaultBoxes()[1] || got[2] != DefaultBoxes()[2] {
		t.Errorf("an unknown kind should take the default: %v", got)
	}
	if got[3] != "depth" {
		t.Errorf("a normal kind was lost: %v", got)
	}
}

func pathSnapshot(now time.Time, path string, v float64, units string, age, maxAge time.Duration) signalk.Snapshot {
	return signalk.Snapshot{
		Connected: true, LastMessage: now,
		Own: signalk.Own{Watched: map[string]signalk.PathReading{
			path: {Reading: signalk.Reading{V: v, At: now.Add(-age)}, MaxAge: maxAge, Units: units},
		}},
	}
}

func TestPathMetricShowsFreshValuesAndDashesForStaleOnes(t *testing.T) {
	now := time.Now()
	id := PathKindID("environment.outside.pressure")
	e := Env{Units: metricUnits}

	m := boxMetric(id, pathSnapshot(now, "environment.outside.pressure", 101300, "", time.Second, 30*time.Second).Own, now, e, nil)
	if !m.ok || m.value != "1013" || m.unit != "hPa" || m.label != "OUTSIDE PRESSURE" {
		t.Errorf("fresh: %+v", m)
	}
	// Older than the path's own rhythm allows: dashes, not a frozen number.
	m = boxMetric(id, pathSnapshot(now, "environment.outside.pressure", 101300, "", time.Minute, 30*time.Second).Own, now, e, nil)
	if m.ok {
		t.Errorf("a stale value is still shown: %+v", m)
	}
	// Not watched, or never sent: dashes.
	if m := boxMetric(id, signalk.Own{}, now, e, nil); m.ok || m.label != "OUTSIDE PRESSURE" {
		t.Errorf("an unwatched path: %+v", m)
	}
	// The server's units are used when it gave them.
	m = boxMetric(PathKindID("vendor.thing"), pathSnapshot(now, "vendor.thing", 3, "m/s", 0, time.Minute).Own, now, Env{Units: nauticalUnits}, nil)
	if m.value != "5.8" || m.unit != "kn" {
		t.Errorf("a speed by the server's say-so: %+v", m)
	}
}

func TestSpeedWidgetCanShowAnyKind(t *testing.T) {
	now := time.Now()
	for id, want := range map[string]string{"sog": "SOG", "stw": "STW", "vmgw": "VMG", "depth": "DEPTH", "baro": "PRESSURE", "twd": "TRUE WIND DIR"} {
		src, ok := SpeedFromID(id)
		if !ok {
			t.Errorf("the speed widget should be able to show %s", id)
			continue
		}
		if src.Label() != want {
			t.Errorf("%s: label %q, want %q", id, src.Label(), want)
		}
		if src.ID() != id {
			t.Errorf("%s: ID() = %q", id, src.ID())
		}
	}
	if src, ok := SpeedFromID(""); !ok || src != SpeedSOG {
		t.Error("empty means the default, SOG")
	}
	if _, ok := SpeedFromID(BoxAIS); ok {
		t.Error("the closest-AIS box is not a single number: the widget cannot show it")
	}
	for _, bad := range []string{"nope", "path:a b", "path:"} {
		if _, ok := SpeedFromID(bad); ok {
			t.Errorf("%q should be refused", bad)
		}
	}
	if src, ok := SpeedFromID(PathKindID("environment.outside.pressure")); !ok || src.Label() != "OUTSIDE PRESSURE" {
		t.Errorf("a path: %v %v", src, ok)
	}

	// Tapping goes round the three speeds, and brings anything else back to SOG.
	if SpeedSOG.Next() != SpeedSTW || SpeedSTW.Next() != SpeedVMG || SpeedVMG.Next() != SpeedSOG {
		t.Error("the tap cycle is SOG, STW, VMG")
	}
	if s, _ := SpeedFromID("depth"); s.Next() != SpeedSOG {
		t.Error("a tap on a widget set to something else should go back to SOG")
	}

	// The widget draws what it was set to, with the label in solid black.
	snap := signalk.Snapshot{Connected: true, LastMessage: now, Own: signalk.Own{
		Depth: signalk.Reading{V: 12.3, At: now}, Heading: signalk.Reading{V: 1, At: now},
		Watched: map[string]signalk.PathReading{"environment.outside.pressure": {Reading: signalk.Reading{V: 101300, At: now}, MaxAge: time.Minute}},
	}}
	depth, _ := SpeedFromID("depth")
	m := speedMetricWith(snap.Own, now, Env{Units: metricUnits, Speed: depth}, nil)
	if m.value != "12.3" || m.unit != "m" || m.label != "DEPTH" || !m.liveLabel || !m.ok {
		t.Errorf("depth widget: %+v", m)
	}
	path, _ := SpeedFromID(PathKindID("environment.outside.pressure"))
	m = speedMetricWith(snap.Own, now, Env{Units: metricUnits, Speed: path}, nil)
	if m.value != "1013" || m.unit != "hPa" || !m.liveLabel {
		t.Errorf("path widget: %+v", m)
	}
}

func TestLongLabelsStayInTheirBox(t *testing.T) {
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	r := image.Rect(0, 100, 536, 400)
	drawMetric(c, r, metric{label: "ELECTRICAL BATTERIES HOUSE CAPACITY STATE OF CHARGE", value: "90", unit: "%", ok: true}, 0)
	if over := inked(c, image.Rect(r.Max.X-20, r.Min.Y, r.Max.X+200, r.Min.Y+100)); over != 0 {
		t.Errorf("the label runs out of its box: %d dark pixels past the edge", over)
	}
	if inked(c, image.Rect(r.Min.X+40, r.Min.Y+20, r.Min.X+300, r.Min.Y+90)) == 0 {
		t.Error("the label is missing")
	}
}
