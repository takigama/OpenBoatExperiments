package pages

import (
	"bytes"
	"image"
	"math"
	"testing"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

var compassNow = time.Unix(100000, 0)

// base is a boat with live heading, speed and depth - and nothing else.
func base() signalk.Snapshot {
	fresh := func(v float64) signalk.Reading { return signalk.Reading{V: v, At: compassNow} }
	return signalk.Snapshot{
		Connected: true, LastMessage: compassNow,
		Own: signalk.Own{Heading: fresh(0.5), SOG: fresh(3), Depth: fresh(12)},
	}
}

func renderCompass(t *testing.T, s signalk.Snapshot) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	Compass(c, s, compassNow, Env{Units: units.Settings{Preset: units.PresetMetric}})
	return c
}

// inked counts non-white pixels in r (anything but background, so greyed-out
// "stale" drawing counts too).
func inked(c *render.Canvas, r image.Rectangle) int {
	n := 0
	for y := r.Min.Y; y < r.Max.Y; y++ {
		for x := r.Min.X; x < r.Max.X; x++ {
			if c.Img.GrayAt(x, y).Y < 200 {
				n++
			}
		}
	}
	return n
}

// The corner areas, inset a little so the compass ring never touches them.
var (
	topRight    = image.Rect(890, 104, 1050, 230)
	bottomRight = image.Rect(890, 990, 1050, 1140)
)

func TestOptionalWidgetsAreAbsentUntilTheServerSendsThem(t *testing.T) {
	c := renderCompass(t, base())
	if n := inked(c, topRight); n != 0 {
		t.Errorf("top-right has %d inked pixels with no temperature - nothing should be drawn", n)
	}
	if n := inked(c, bottomRight); n != 0 {
		t.Errorf("bottom-right has %d inked pixels with no fuel tanks - nothing should be drawn", n)
	}
}

func TestWaterTempAppearsOnceSeen(t *testing.T) {
	s := base()
	s.Own.WaterTemp = signalk.Reading{V: 297.15, At: compassNow}
	if n := inked(renderCompass(t, s), topRight); n == 0 {
		t.Error("a live water temperature should be drawn top-right")
	}
	// Seen, but old: still shown (as "--"), so the loss is visible rather
	// than the widget silently vanishing.
	s.Own.WaterTemp.At = compassNow.Add(-10 * time.Minute)
	if n := inked(renderCompass(t, s), topRight); n == 0 {
		t.Error("a stale temperature should still be drawn, greyed out")
	}
}

func TestFuelGaugesOnePerTank(t *testing.T) {
	one := base()
	one.Own.Fuel = []signalk.Tank{{ID: "0", Level: signalk.Reading{V: 0.8, At: compassNow}}}
	two := base()
	two.Own.Fuel = []signalk.Tank{
		{ID: "0", Level: signalk.Reading{V: 0.8, At: compassNow}},
		{ID: "1", Level: signalk.Reading{V: 0.4, At: compassNow}},
	}
	n1 := inked(renderCompass(t, one), bottomRight)
	n2 := inked(renderCompass(t, two), bottomRight)
	if n1 == 0 {
		t.Error("one tank should draw a gauge")
	}
	if n2 <= n1 {
		t.Errorf("two tanks (%d inked) should draw more than one (%d)", n2, n1)
	}
}

func TestFuelLevelChangesTheFill(t *testing.T) {
	at := func(level float64) int {
		s := base()
		s.Own.Fuel = []signalk.Tank{{ID: "0", Level: signalk.Reading{V: level, At: compassNow}}}
		return inked(renderCompass(t, s), bottomRight)
	}
	if empty, full := at(0.05), at(0.95); full <= empty {
		t.Errorf("a fuller tank should ink more: 5%% = %d, 95%% = %d", empty, full)
	}
	// Out-of-range readings are clamped instead of drawing outside the bar.
	if over, full := at(1.7), at(1.0); over != full {
		t.Errorf("a level above 1 should draw like 100%%: %d vs %d", over, full)
	}
	if under, empty := at(-0.3), at(0); under != empty {
		t.Errorf("a level below 0 should draw like empty: %d vs %d", under, empty)
	}
}

func differs(a, b *render.Canvas) bool { return !bytes.Equal(a.Img.Pix, b.Img.Pix) }

func TestWindPointerOnlyWhileLive(t *testing.T) {
	without := renderCompass(t, base())

	live := base()
	live.Own.AWA = signalk.Reading{V: math.Pi / 2, At: compassNow} // wind on the starboard beam
	if !differs(renderCompass(t, live), without) {
		t.Error("a live apparent wind angle should draw a pointer")
	}

	stale := base()
	stale.Own.AWA = signalk.Reading{V: math.Pi / 2, At: compassNow.Add(-time.Minute)}
	if differs(renderCompass(t, stale), without) {
		t.Error("a stale wind angle must not draw a pointer - old data would look live")
	}
}

func TestWindPointerFollowsTheAngle(t *testing.T) {
	at := func(angle float64) *render.Canvas {
		s := base()
		s.Own.AWA = signalk.Reading{V: angle, At: compassNow}
		return renderCompass(t, s)
	}
	// Mirror-image winds put the pointer on opposite sides of the bow.
	port, stbd := at(-math.Pi/2), at(math.Pi/2)
	if !differs(port, stbd) {
		t.Error("wind from port and wind from starboard must look different")
	}
	// The pointer is heavy black ink on the rim: its side of the card has
	// more of it than the other.
	left, right := image.Rect(0, 560, 120, 700), image.Rect(950, 560, 1072, 700)
	if inked(port, left) <= inked(stbd, left) || inked(stbd, right) <= inked(port, right) {
		t.Errorf("pointer not on the expected side: left %d/%d, right %d/%d",
			inked(port, left), inked(stbd, left), inked(port, right), inked(stbd, right))
	}
}
