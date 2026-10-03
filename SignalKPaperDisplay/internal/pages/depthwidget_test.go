package pages

import (
	"image"
	"testing"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
)

func TestDepthWidgetSettings(t *testing.T) {
	for _, id := range []string{"", "depth"} {
		if s, ok := DepthFromID(id); !ok || s != "" {
			t.Errorf("%q is the default (empty), got %q %v", id, s, ok)
		}
	}
	if DepthWidgetID("") != "depth" || DepthWidgetID("stw") != "stw" {
		t.Error("DepthWidgetID should name the default depth")
	}
	for _, id := range []string{"sog", "baro", "batv", "path:environment.outside.pressure"} {
		if s, ok := DepthFromID(id); !ok || s != id {
			t.Errorf("the depth widget should be able to show %q, got %q %v", id, s, ok)
		}
	}
	for _, bad := range []string{BoxAIS, "nope", "path:a b", "path:"} {
		if _, ok := DepthFromID(bad); ok {
			t.Errorf("%q should be refused", bad)
		}
	}
}

func depthSnapshot(now time.Time) signalk.Snapshot {
	return signalk.Snapshot{Connected: true, LastMessage: now, Own: signalk.Own{
		Heading: signalk.Reading{V: 1, At: now},
		Depth:   signalk.Reading{V: 12.3, At: now},
		SOG:     signalk.Reading{V: 3, At: now},
		Extra:   map[string]signalk.Reading{"environment.outside.pressure": {V: 101300, At: now}},
		Watched: map[string]signalk.PathReading{"environment.outside.pressure": {Reading: signalk.Reading{V: 101300, At: now}, MaxAge: time.Minute}},
	}}
}

func renderDepth(t *testing.T, s signalk.Snapshot, e Env) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	Compass(c, s, time.Now(), e)
	return c
}

func TestCompassDepthBoxShowsDepthUnlessSetOtherwise(t *testing.T) {
	now := time.Now()
	s := depthSnapshot(now)
	box := DepthBoxRect(image.Rect(0, 0, 1072, 1448))

	// The default draws exactly what the box has always drawn, in the same place.
	zero := renderDepth(t, s, Env{Units: metricUnits})
	explicit := renderDepth(t, s, Env{Units: metricUnits, Depth: ""})
	if differs(zero, explicit) {
		t.Error("the default must be stable")
	}
	if inked(zero, box) == 0 {
		t.Fatal("no depth box drawn")
	}
	// It sits beside the speed box, in the strip at the bottom.
	sp := SpeedBoxRect(image.Rect(0, 0, 1072, 1448))
	if box.Min.X <= sp.Max.X-4 || box.Min.Y != sp.Min.Y || box.Max.Y != sp.Max.Y || box.Max.X != 1072 {
		t.Errorf("depth box %v is not the right half of the strip the speed box %v is the left of", box, sp)
	}

	// Set to something else it draws that, and only the box changes.
	other := renderDepth(t, s, Env{Units: metricUnits, Depth: "baro"})
	if !differs(zero, other) {
		t.Fatal("setting the widget to air pressure changed nothing")
	}
	outside := image.Rect(0, 0, 1072, box.Min.Y-2)
	for y := outside.Min.Y; y < outside.Max.Y; y++ {
		for x := 0; x < 1072; x++ {
			if zero.Img.GrayAt(x, y).Y != other.Img.GrayAt(x, y).Y {
				t.Fatalf("pixel %d,%d outside the depth box changed", x, y)
			}
		}
	}
	for y := box.Min.Y; y < box.Max.Y; y++ {
		for x := 0; x < box.Min.X; x++ {
			if zero.Img.GrayAt(x, y).Y != other.Img.GrayAt(x, y).Y {
				t.Fatalf("pixel %d,%d in the speed box changed", x, y)
			}
		}
	}

	// A raw SignalK path works too, and the metric is what a Nav box would show.
	m := boxMetric(DepthWidgetID("path:environment.outside.pressure"), s.Own, now, Env{Units: metricUnits}, nil)
	if m.value != "1013" || m.unit != "hPa" || !m.ok {
		t.Errorf("a path in the depth widget: %+v", m)
	}
	// Depth stale: dashes, like always.
	s.Own.Depth.At = now.Add(-time.Minute)
	if m := boxMetric(DepthWidgetID(""), s.Own, now, Env{Units: metricUnits}, nil); m.ok || m.label != "DEPTH" || m.unit != "m" {
		t.Errorf("stale depth: %+v", m)
	}
}
