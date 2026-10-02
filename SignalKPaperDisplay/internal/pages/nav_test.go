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

func renderNav(t *testing.T, s signalk.Snapshot) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	Nav(c, s, compassNow, Env{Units: units.Settings{Preset: units.PresetMetric}})
	return c
}

// A boat that's moving, with a fix and wind, and a few ships around it.
func navBoat(ships int) signalk.Snapshot {
	s := withShipAhead(1500, 4, 0.2)
	fresh := func(v float64) signalk.Reading { return signalk.Reading{V: v, At: compassNow} }
	s.Own.STW = fresh(4)
	s.Own.TWD = fresh(0.2 + math.Pi/3)
	s.Targets = nil
	for i := 0; i < ships; i++ {
		s.Targets = append(s.Targets, signalk.Target{
			ID: "urn:mrn:imo:mmsi:23500000" + string(rune('1'+i)), Name: []string{"Triteia", "Aura", "Traviata", "To Keel a Sunset"}[i],
			Pos: signalk.Position{Lat: testLat + float64(1500+i*1200)/mPerDeg, Lon: testLon + float64(i)*0.004, At: compassNow},
			SOG: fresh(0), COG: fresh(0),
		})
	}
	return s
}

func TestNavHasEightBoxesAndEachShowsSomething(t *testing.T) {
	c := renderNav(t, navBoat(3))
	b := c.Bounds()
	if NavBoxes != 8 {
		t.Fatalf("NavBoxes = %d, want 8", NavBoxes)
	}
	for i := 0; i < NavBoxes; i++ {
		cell := navCell(b, i)
		inner := cell.Inset(8)
		if inked(c, inner) == 0 {
			t.Errorf("box %d is empty", i)
		}
	}
	// Boxes tile the area under the header: two across, four down.
	if navCell(b, 1).Min.X != navCell(b, 0).Max.X || navCell(b, 2).Min.Y != navCell(b, 0).Max.Y {
		t.Error("the boxes should sit edge to edge")
	}
	if navCell(b, NavBoxes-1).Max.X != b.Dx() {
		t.Errorf("the grid should span the full width, last box ends at %d of %d", navCell(b, NavBoxes-1).Max.X, b.Dx())
	}
	if last := navCell(b, NavBoxes-1).Max.Y; last > b.Dy() || last < b.Dy()-4 {
		t.Errorf("the grid should run to the bottom of the screen, ends at %d of %d", last, b.Dy())
	}
}

func TestNavClosestAISShowsOnlyTheNearest(t *testing.T) {
	cell := navCell(image.Rect(0, 0, 1072, 1448), 5).Inset(8)
	none := inked(renderNav(t, navBoat(0)), cell)
	one := inked(renderNav(t, navBoat(1)), cell)
	three := inked(renderNav(t, navBoat(3)), cell)
	if !(none > 0 && one > 0 && differs(renderNav(t, navBoat(0)), renderNav(t, navBoat(1)))) {
		t.Errorf("a contact should change the box: none=%d one=%d", none, one)
	}
	// More ships further off change nothing: only the closest is shown.
	if one != three || !sameInRect(renderNav(t, navBoat(1)), renderNav(t, navBoat(3)), cell) {
		t.Errorf("only the closest contact is shown: one=%d three=%d", one, three)
	}
}

// sameInRect reports whether two renders are identical inside r.
func sameInRect(a, b *render.Canvas, r image.Rectangle) bool {
	for y := r.Min.Y; y < r.Max.Y; y++ {
		for x := r.Min.X; x < r.Max.X; x++ {
			if a.Img.GrayAt(x, y).Y != b.Img.GrayAt(x, y).Y {
				return false
			}
		}
	}
	return true
}

func TestNavClosestAISNamesTheShipAndWithoutAFixShowsDashes(t *testing.T) {
	s := navBoat(3)
	s.Own.Pos.At = compassNow.Add(-time.Minute)
	cell := navCell(image.Rect(0, 0, 1072, 1448), 5).Inset(8)
	withFix := inked(renderNav(t, navBoat(3)), cell)
	noFix := inked(renderNav(t, s), cell)
	if noFix >= withFix {
		t.Errorf("with no fix the box must not show a ship: %d vs %d", noFix, withFix)
	}
	// A long name is cut to fit the label line before the diamond, not run off the box.
	long := navBoat(1)
	long.Targets[0].Name = "AN EXTREMELY LONG SHIP NAME THAT CANNOT FIT"
	c := renderNav(t, long)
	cell5 := navCell(c.Bounds(), 5)
	beyond := image.Rect(cell5.Max.X-30, cell5.Min.Y+30, cell5.Max.X, cell5.Min.Y+90) // the label line, past the diamond
	if inked(c, beyond) != 0 {
		t.Errorf("the name ran %d px of ink into the right margin", inked(c, beyond))
	}
}

func TestVMG(t *testing.T) {
	fresh := func(v float64) signalk.Reading { return signalk.Reading{V: v, At: compassNow} }
	own := signalk.Own{Heading: fresh(0), TWD: fresh(math.Pi / 3), STW: fresh(5), SOG: fresh(9)}

	// 60 degrees off the wind at 5 m/s: 5*cos(60) = 2.5. The speed through
	// the water is used, not the (here very different) speed over ground.
	if v, ok := vmg(own, compassNow); !ok || math.Abs(v-2.5) > 1e-9 {
		t.Errorf("vmg = %v, %v; want 2.5", v, ok)
	}
	// Running downwind counts too: it's the speed made good along the wind line.
	own.TWD = fresh(math.Pi * 2 / 3)
	if v, ok := vmg(own, compassNow); !ok || math.Abs(v-2.5) > 1e-9 {
		t.Errorf("downwind vmg = %v, %v; want 2.5", v, ok)
	}
	// Beam on, nothing is made good to the wind.
	own.TWD = fresh(math.Pi / 2)
	if v, _ := vmg(own, compassNow); math.Abs(v) > 1e-9 {
		t.Errorf("beam reach vmg = %v, want 0", v)
	}

	// No STW: fall back to speed over ground.
	own.TWD = fresh(math.Pi / 3)
	own.STW = signalk.Reading{}
	if v, ok := vmg(own, compassNow); !ok || math.Abs(v-4.5) > 1e-9 {
		t.Errorf("fallback vmg = %v, %v; want 4.5", v, ok)
	}
	// It needs wind, a heading and a speed, all current.
	for name, mut := range map[string]func(*signalk.Own){
		"no wind":      func(o *signalk.Own) { o.TWD = signalk.Reading{} },
		"stale wind":   func(o *signalk.Own) { o.TWD.At = compassNow.Add(-time.Minute) },
		"no heading":   func(o *signalk.Own) { o.Heading = signalk.Reading{} },
		"no speed":     func(o *signalk.Own) { o.SOG = signalk.Reading{} },
		"stale speeds": func(o *signalk.Own) { o.SOG.At = compassNow.Add(-time.Minute) },
	} {
		o := own
		mut(&o)
		if _, ok := vmg(o, compassNow); ok {
			t.Errorf("%s: vmg should be unavailable", name)
		}
	}
}

func TestNavCOGShowsDashesWhileStopped(t *testing.T) {
	cell := navCell(image.Rect(0, 0, 1072, 1448), 3).Inset(8)
	moving := navBoat(0)
	stopped := navBoat(0)
	stopped.Own.SOG = signalk.Reading{V: 0.05, At: compassNow}
	a, b := renderNav(t, moving), renderNav(t, stopped)
	if !differs(a, b) {
		t.Fatal("test setup: stopping should change the picture")
	}
	if inked(b, cell) >= inked(a, cell) {
		t.Errorf("a stopped boat's COG box should show short dashes, not a course: %d vs %d", inked(b, cell), inked(a, cell))
	}
}

func TestNavValuesAreAllTheSameSize(t *testing.T) {
	// The heading "088" and the depth "7.2" are different widths, but both
	// should be drawn at the same size: compare the height of their digits.
	c := renderNav(t, navBoat(0))
	height := func(cell image.Rectangle) int {
		minY, maxY := 1<<30, 0
		for y := cell.Min.Y + 100; y < cell.Max.Y; y++ { // below the label
			for x := cell.Min.X; x < cell.Max.X; x++ {
				if c.Img.GrayAt(x, y).Y < 100 {
					minY, maxY = min(minY, y), max(maxY, y)
				}
			}
		}
		return maxY - minY
	}
	b := c.Bounds()
	hSOG, hDepth := height(navCell(b, 0).Inset(4)), height(navCell(b, 2).Inset(4))
	if d := hSOG - hDepth; d < -12 || d > 12 {
		t.Errorf("value heights differ: SOG %d, depth %d", hSOG, hDepth)
	}
}

func TestNavShowsWhatTheBoxesAreSetTo(t *testing.T) {
	boat := navBoat(0)
	boat.Own.WPDistance = signalk.Reading{V: 3704, At: compassNow}

	draw := func(boxes []string) *render.Canvas {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Nav(c, boat, compassNow, Env{Units: units.Settings{Preset: units.PresetMetric}, Boxes: boxes})
		return c
	}
	def := draw(nil)
	custom := draw([]string{"wpdist"}) // box 1 only; the rest stay at their defaults
	cell := navCell(def.Bounds(), 0)
	if !differs(def, custom) {
		t.Fatal("choosing another kind for box 1 should change the picture")
	}
	if inked(custom, cell.Inset(8)) == 0 {
		t.Error("the chosen box should show its value")
	}
	// Only box 1 changed.
	for i := 1; i < 6; i++ {
		c := navCell(def.Bounds(), i)
		if inked(def, c.Inset(8)) != inked(custom, c.Inset(8)) {
			t.Errorf("box %d changed though only box 1 was reassigned", i+1)
		}
	}
	// The AIS list can go in any box.
	ais := draw([]string{BoxAIS, "hdg", "depth", "cog", "vmgw", "sog"})
	if inked(ais, navCell(def.Bounds(), 0).Inset(8)) == 0 {
		t.Error("the AIS list should work in box 1")
	}
}
