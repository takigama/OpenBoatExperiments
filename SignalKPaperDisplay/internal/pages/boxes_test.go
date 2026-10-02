package pages

import (
	"math"
	"strings"
	"testing"
	"time"

	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func rd(v float64) signalk.Reading { return signalk.Reading{V: v, At: compassNow} }

func metricEnv() Env { return Env{Units: units.Settings{Preset: units.PresetMetric}} }

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
	own := signalk.Own{
		Heading: rd(0.1), COG: rd(0.2), SOG: rd(3), STW: rd(3), Depth: rd(10), WaterTemp: rd(290),
		AWA: rd(0.5), AWS: rd(5), TWD: rd(1), TWS: rd(6),
		WPBearing: rd(0.3), WPDistance: rd(5000), WPTimeToGo: rd(1800),
	}
	for _, k := range BoxKinds {
		if k.ID == BoxAIS {
			continue
		}
		m := boxMetric(k.ID, own, compassNow, metricEnv())
		if !m.ok || m.value == "" || m.label != k.Label {
			t.Errorf("%s: boxMetric = %+v, want a live value labelled %q", k.ID, m, k.Label)
		}
	}
	if len(DefaultBoxes()) != NavBoxes {
		t.Errorf("defaults have %d boxes, want %d", len(DefaultBoxes()), NavBoxes)
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
	long := NormalizeBoxes([]string{"sog", "sog", "sog", "sog", "sog", "sog", "sog", "sog"})
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
	if m := boxMetric("awa", own, compassNow, metricEnv()); m.value != "45" || m.unit != "port" {
		t.Errorf("wind angle to port: %+v", m)
	}
	own.AWA = rd(0.7854)
	if m := boxMetric("awa", own, compassNow, metricEnv()); m.value != "45" || m.unit != "stbd" {
		t.Errorf("wind angle to starboard: %+v", m)
	}
	// Angles are folded to the short way round: 350 degrees is 10 to port.
	own.AWA = rd(350 * math.Pi / 180)
	if m := boxMetric("awa", own, compassNow, metricEnv()); m.value != "10" || m.unit != "port" {
		t.Errorf("wind angle 350: %+v", m)
	}
	if m := boxMetric("wpdist", own, compassNow, metricEnv()); m.value != "3.70" || m.unit != "km" {
		t.Errorf("distance in metric: %+v", m)
	}
	nautical := Env{Units: units.Settings{Preset: units.PresetNautical}}
	if m := boxMetric("wpdist", own, compassNow, nautical); m.value != "2.00" || m.unit != "nm" {
		t.Errorf("distance in nautical: %+v", m)
	}
	if m := boxMetric("wpttg", own, compassNow, metricEnv()); m.value != "1:30" || m.unit != "h:m" {
		t.Errorf("time to go: %+v", m)
	}
	if m := boxMetric("twd", own, compassNow, metricEnv()); m.value != "180" {
		t.Errorf("true wind direction: %+v", m)
	}
	// Data that never arrived, or has gone stale, is not ok.
	for _, id := range []string{"wpbrg", "wpttw", "vmgwp", "stw", "wtemp"} {
		if m := boxMetric(id, own, compassNow, metricEnv()); m.ok {
			t.Errorf("%s with no data should not be ok: %+v", id, m)
		}
	}
	own.WPDistance.At = compassNow.Add(-time.Minute)
	if m := boxMetric("wpdist", own, compassNow, metricEnv()); m.ok {
		t.Error("a stale waypoint distance must show dashes")
	}
}
