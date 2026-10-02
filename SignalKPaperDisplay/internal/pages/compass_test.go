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

// The waypoint pointer for a bearing 90 degrees to starboard of a boat
// heading north sits on the right-hand rim, at (cx+r, cy) = (1002, 622).
func withWaypoint(bearing float64) signalk.Snapshot {
	s := base()
	s.Own.Heading = signalk.Reading{V: 0, At: compassNow}
	s.Own.WPBearing = signalk.Reading{V: bearing, At: compassNow}
	return s
}

func TestWaypointPointerAppearsAtItsBearingAndOnlyWithData(t *testing.T) {
	rim := image.Rect(930, 560, 1060, 690)
	without := renderCompass(t, noWaypoint(withWaypoint(math.Pi/2)))
	with := renderCompass(t, withWaypoint(math.Pi/2))
	if inked(with, rim) <= inked(without, rim) {
		t.Errorf("a waypoint to starboard should add ink on the right rim: %d vs %d", inked(with, rim), inked(without, rim))
	}
	// Elsewhere it isn't there: the same waypoint dead astern leaves the right rim alone.
	astern := renderCompass(t, withWaypoint(math.Pi))
	if inked(astern, rim) != inked(without, rim) {
		t.Error("a waypoint astern should not draw on the starboard rim")
	}
	stale := withWaypoint(math.Pi / 2)
	stale.Own.WPBearing.At = compassNow.Add(-time.Minute)
	if differs(renderCompass(t, stale), without) {
		t.Error("a stale waypoint bearing must not be drawn")
	}
	noHeading := withWaypoint(math.Pi / 2)
	noHeading.Own.Heading.At = compassNow.Add(-time.Minute)
	if differs(renderCompass(t, noHeading), renderCompass(t, noWaypoint(noHeading))) {
		t.Error("with no live heading the bearing has nothing to be relative to, so no pointer")
	}
}

func noWaypoint(s signalk.Snapshot) signalk.Snapshot {
	s.Own.WPBearing = signalk.Reading{}
	return s
}

func TestWaypointPointerIsHollowAndPointsOutward(t *testing.T) {
	c := renderCompass(t, withWaypoint(math.Pi/2))
	// Pointing outward: its tip is outside the ring (x > 1002+8) and its
	// wide base inside (x < 1002-40).
	col := func(x int) int { return inked(c, image.Rect(x, 580, x+1, 665)) }
	if col(1030) == 0 {
		t.Error("the tip should reach outside the ring")
	}
	if col(940) <= col(1030) {
		t.Errorf("the base should be wider than the tip: %d vs %d", col(940), col(1030))
	}
	// Hollow: the middle of the arrowhead is paper, not ink.
	if g := c.Img.GrayAt(980, 622).Y; g < 200 {
		t.Errorf("the pointer's middle is %d; it should be hollow", g)
	}
}

func TestWaypointPointerHasAWUnderIt(t *testing.T) {
	// Turned to starboard the pointer's base is the vertical line at x=926
	// (r-76 from the card's centre). The W is on the base side - to its left
	// - turned a quarter so its top faces the triangle, and 2px clear of it.
	c := renderCompass(t, withWaypoint(math.Pi/2))
	none := renderCompass(t, noWaypoint(withWaypoint(math.Pi/2)))
	label := image.Rect(860, 596, 926, 650)
	if inked(c, label) <= inked(none, label) {
		t.Errorf("a W should sit just inside the pointer's base: %d vs %d inked", inked(c, label), inked(none, label))
	}
	// The gap: the pixel columns between the W's top and the pointer's base
	// hold none of the W (ticks that were already there are not counted).
	gap := image.Rect(924, 612, 926, 632)
	if inked(c, gap) != inked(none, gap) {
		t.Errorf("the 2px gap under the pointer has %d inked pixels added", inked(c, gap)-inked(none, gap))
	}
	// And the W reaches right up to the gap: its ink starts within a pixel or
	// two of it, not floating further in.
	nearGap := image.Rect(920, 596, 924, 650)
	if inked(c, nearGap) <= inked(none, nearGap) {
		t.Error("the W should come right up to the 2px gap")
	}

	// Dead ahead the pointer is at the top and the W is below its base, upright.
	top := renderCompass(t, withWaypoint(0))
	noTop := renderCompass(t, noWaypoint(withWaypoint(0)))
	below := image.Rect(500, 236, 572, 290) // inside the base at y = 622-466+76 = 232
	if inked(top, below) <= inked(noTop, below) {
		t.Error("with the pointer at the top the W should be below it")
	}
	// At the bottom the whole marker is turned over: the W is above the base,
	// upside-down (an M).
	bottom := renderCompass(t, withWaypoint(math.Pi))
	noBottom := renderCompass(t, noWaypoint(withWaypoint(math.Pi)))
	above := image.Rect(500, 950, 572, 1010) // base at y = 622+466-76 = 1012
	if inked(bottom, above) <= inked(noBottom, above) {
		t.Error("with the pointer at the bottom the W should be above it, turned over")
	}
}

func TestWaypointLabelTurnsWithThePointerAtEveryAngle(t *testing.T) {
	// The marker is rigid: at every angle the W is on the base side, centred
	// on the pointer's axis.
	for deg := 0; deg < 360; deg += 15 {
		rad := float64(deg) * math.Pi / 180
		c := renderCompass(t, withWaypoint(rad))
		none := renderCompass(t, noWaypoint(withWaypoint(rad)))
		if !differs(c, none) {
			t.Fatalf("angle %d: nothing drawn", deg)
		}
		ux, uy := math.Sin(rad), -math.Cos(rad)
		d := 466.0 - 76 - 2 - 20 // the ink's middle, about 20px in from the 2px gap
		x, y := 536+int(ux*d), 622+int(uy*d)
		changed := 0
		for yy := y - 34; yy < y+34; yy++ {
			for xx := x - 34; xx < x+34; xx++ {
				if c.Img.GrayAt(xx, yy).Y != none.Img.GrayAt(xx, yy).Y {
					changed++
				}
			}
		}
		if changed < 80 {
			t.Errorf("angle %d: only %d pixels differ near (%d,%d); no W", deg, changed, x, y)
		}
	}
}
