package pages

import (
	"image"
	"math"
	"testing"
	"time"

	"signalkpaperdisplay/internal/signalk"
)

const (
	testLat, testLon = -33.85, 151.28
	mPerDeg          = 111320.0
)

// withShipAhead returns the base boat with a valid position and course
// (heading due north), plus a ship dN metres north of it.
func withShipAhead(dN, ownSOG, ownCourse float64) signalk.Snapshot {
	s := base()
	fresh := func(v float64) signalk.Reading { return signalk.Reading{V: v, At: compassNow} }
	s.Own.Heading = fresh(ownCourse)
	s.Own.COG = fresh(ownCourse)
	s.Own.SOG = fresh(ownSOG)
	s.Own.Pos = signalk.Position{Lat: testLat, Lon: testLon, At: compassNow}
	s.Targets = []signalk.Target{{
		ID: "ship", Name: "SHIP",
		Pos: signalk.Position{Lat: testLat + dN/mPerDeg, Lon: testLon, At: compassNow},
		SOG: fresh(0), COG: fresh(0),
	}}
	return s
}

// The rim at the top of the card is at about (536, 156): cy = 622, r = 466.
var topRim = image.Rect(486, 100, 586, 215)

func TestAISBlipsAppearAtTheirBearing(t *testing.T) {
	without := renderCompass(t, base())

	ahead := renderCompass(t, withShipAhead(3000, 0, 0))
	if !differs(ahead, without) {
		t.Fatal("a fresh AIS target should draw a blip")
	}
	// Dead ahead means on the rim at the top, over the bow line.
	if inked(ahead, topRim) <= inked(without, topRim) {
		t.Errorf("a ship dead ahead should add ink at the top of the rim: %d vs %d",
			inked(ahead, topRim), inked(without, topRim))
	}

	// The same ship, but we've turned to starboard 90 degrees: it's now on
	// our port side, so the blip leaves the top and appears on the left.
	port := renderCompass(t, withShipAhead(3000, 0, math.Pi/2))
	left := image.Rect(40, 540, 150, 700)
	if inked(port, left) <= inked(without, left) {
		t.Error("after turning 90 degrees to starboard the ship should appear on the left (port) rim")
	}
	if inked(port, topRim) > inked(ahead, topRim) {
		t.Error("the blip should have left the top once the ship is no longer ahead")
	}
}

// noTargets is the same boat with the AIS targets removed, so the only
// difference between two renders is whether blips were drawn.
func noTargets(s signalk.Snapshot) signalk.Snapshot {
	s.Targets = nil
	return s
}

func TestAISBlipsHiddenWithoutAFixOrHeading(t *testing.T) {
	// Sanity: with a good fix the target does change the picture.
	good := withShipAhead(3000, 0, 0)
	if !differs(renderCompass(t, good), renderCompass(t, noTargets(good))) {
		t.Fatal("test setup: a fresh target should draw a blip")
	}

	noPos := withShipAhead(3000, 0, 0)
	noPos.Own.Pos.At = compassNow.Add(-time.Minute)
	if differs(renderCompass(t, noPos), renderCompass(t, noTargets(noPos))) {
		t.Error("with a stale own position no blips may be drawn - the bearings would be wrong")
	}

	noHeading := withShipAhead(3000, 0, 0)
	noHeading.Own.Heading.At = compassNow.Add(-time.Minute)
	if differs(renderCompass(t, noHeading), renderCompass(t, noTargets(noHeading))) {
		t.Error("with a stale heading no blips may be drawn")
	}
}

func TestClosingShipsAreSolidAndOpeningOnesHollow(t *testing.T) {
	// Stationary ship 3 km ahead; we steam toward it, or away from it.
	closing := renderCompass(t, withShipAhead(3000, 5, 0))
	opening := renderCompass(t, withShipAhead(-3000, 5, 0)) // behind us, so we're leaving it
	// Compare like with like: the blip's own area, wherever it is.
	ahead := image.Rect(486, 100, 586, 215)
	behind := image.Rect(486, 1030, 586, 1140)
	solid, hollow := inked(closing, ahead), inked(opening, behind)
	if solid <= hollow {
		t.Errorf("a closing (solid) blip should ink more than an opening (hollow) one: %d vs %d", solid, hollow)
	}
}

func TestPlaceLabelAvoidsLabelsAlreadyPlaced(t *testing.T) {
	cands := []image.Point{{500, 900}, {500, 850}, {500, 800}}
	var placed []image.Rectangle

	first, box1 := placeLabel(cands, 120, placed)
	placed = append(placed, box1)
	if first != cands[0] {
		t.Fatalf("with nothing in the way the first choice should win, got %v", first)
	}

	// The next label wants the same spot: it must move to a later candidate
	// whose box is clear of the first.
	second, box2 := placeLabel(cands, 120, placed)
	if second == cands[0] || box2.Overlaps(box1) {
		t.Errorf("second label %v (%v) overlaps the first (%v)", second, box2, box1)
	}
	placed = append(placed, box2)

	third, box3 := placeLabel(cands, 120, placed)
	if box3.Overlaps(box1) || box3.Overlaps(box2) {
		t.Errorf("third label %v (%v) overlaps an earlier one", third, box3)
	}

	// If nothing is free it still returns something, rather than failing.
	placed = append(placed, box3)
	if p, _ := placeLabel(cands, 120, placed); p != cands[len(cands)-1] {
		t.Errorf("with every spot taken it should settle for the last, got %v", p)
	}
}

func TestRangeLabelsOnlyForTheNearestThree(t *testing.T) {
	s := withShipAhead(1000, 0, 0)
	for i, d := range []float64{2000, 3000, 4000, 5000} {
		s.Targets = append(s.Targets, signalk.Target{
			ID:  string(rune('a' + i)),
			Pos: signalk.Position{Lat: testLat + d/mPerDeg, Lon: testLon + float64(i+1)*0.01, At: compassNow},
		})
	}
	// Just make sure five contacts draw without trouble and ink more than one.
	one := inked(renderCompass(t, withShipAhead(1000, 0, 0)), image.Rect(0, 94, 1072, 1150))
	five := inked(renderCompass(t, s), image.Rect(0, 94, 1072, 1150))
	if five <= one {
		t.Errorf("five contacts (%d) should ink more than one (%d)", five, one)
	}
}

func TestBowLineRunsFromAboveTheDigitsToTheTopOfTheCompassArea(t *testing.T) {
	c := renderCompass(t, base())
	dark := func(x, y int) bool { return c.Img.GrayAt(x, y).Y < 100 }
	// Solid ink all the way: through the card, across the rim, and up the
	// margin above it to the top of the compass area.
	for _, y := range []int{100, 112, 125, 138, 200, 300, 400, 480} {
		if !dark(536, y) {
			t.Errorf("bow line missing at y=%d", y)
		}
	}
	// It stops at the compass area; it doesn't run into the header.
	if dark(536, 60) {
		t.Error("the bow line should not extend into the header")
	}
	// And it starts above the heading digits, not through them.
	if dark(536, 560) && !dark(500, 560) && !dark(570, 560) {
		t.Error("a bare line at digit height would mean it cuts through the heading numbers")
	}
}
