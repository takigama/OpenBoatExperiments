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

func TestFiveContactsDrawMoreThanOne(t *testing.T) {
	s := withShipAhead(1000, 0, 0)
	for i, d := range []float64{2000, 3000, 4000, 5000} {
		s.Targets = append(s.Targets, signalk.Target{
			ID:  string(rune('a' + i)),
			Pos: signalk.Position{Lat: testLat + d/mPerDeg, Lon: testLon + float64(i+1)*0.01, At: compassNow},
		})
	}
	one := inked(renderCompass(t, withShipAhead(1000, 0, 0)), image.Rect(0, 94, 1072, 1150))
	five := inked(renderCompass(t, s), image.Rect(0, 94, 1072, 1150))
	if five <= one {
		t.Errorf("five contacts (%d) should ink more than one (%d)", five, one)
	}
}

var (
	topLeft    = image.Rect(24, 104, 260, 215)
	bottomLeft = image.Rect(24, 1000, 230, 1140)
)

func TestClosestAISShowsTopLeftOnlyWithAContact(t *testing.T) {
	if n := inked(renderCompass(t, base()), topLeft); n != 0 {
		t.Errorf("no contacts, but %d inked pixels in the top-left", n)
	}
	if n := inked(renderCompass(t, withShipAhead(3000, 0, 0)), topLeft); n == 0 {
		t.Error("a contact should put its name and distance top-left")
	}
	// A contact with no usable own position can't be measured: nothing shown.
	s := withShipAhead(3000, 0, 0)
	s.Own.Pos.At = compassNow.Add(-time.Minute)
	if n := inked(renderCompass(t, s), topLeft); n != 0 {
		t.Errorf("with no own position the top-left should be empty, got %d", n)
	}
}

func TestClosestAISIsTheNearestAndFallsBackToTheMMSI(t *testing.T) {
	near := withShipAhead(1000, 0, 0)
	far := withShipAhead(1000, 0, 0)
	far.Targets = append(far.Targets, signalk.Target{
		ID: "urn:mrn:imo:mmsi:235000009", Name: "A VERY LONG SHIP NAME INDEED",
		Pos: signalk.Position{Lat: testLat + 9000/mPerDeg, Lon: testLon, At: compassNow},
	})
	// A farther ship must not change which one is shown.
	if inkNear, inkFar := inked(renderCompass(t, near), topLeft), inked(renderCompass(t, far), topLeft); inkNear != inkFar {
		t.Errorf("a farther ship changed the top-left: %d vs %d", inkNear, inkFar)
	}

	// A long name is shortened to the corner rather than running into the card.
	long := withShipAhead(1000, 0, 0)
	long.Targets[0].Name = "A VERY LONG SHIP NAME INDEED AND THEN SOME"
	c := renderCompass(t, long)
	// The name's line is at y 108..132; beyond x=440 there must be nothing
	// of it (the ship dead ahead has its diamond at x=500..570).
	if n := inked(c, image.Rect(440, 108, 495, 134)); n != 0 {
		t.Errorf("long name spilled %d pixels past the corner", n)
	}
}

func TestWindSpeedShowsBottomLeftOnceSeen(t *testing.T) {
	if n := inked(renderCompass(t, base()), bottomLeft); n != 0 {
		t.Errorf("no wind data, but %d inked pixels bottom-left", n)
	}
	s := base()
	s.Own.AWS = signalk.Reading{V: 6, At: compassNow}
	live := renderCompass(t, s)
	if inked(live, bottomLeft) == 0 {
		t.Fatal("wind speed should appear bottom-left")
	}
	s.Own.AWS.At = compassNow.Add(-time.Minute)
	stale := renderCompass(t, s)
	if inked(stale, bottomLeft) == 0 {
		t.Error("stale wind speed should show \"--\", not vanish")
	}
	if !differs(live, stale) {
		t.Error("stale wind speed should look different to live")
	}
}

func TestBowLineRunsFromAboveTheDigitsToTheTopOfTheCompassArea(t *testing.T) {
	c := renderCompass(t, base())
	dark := func(x, y int) bool { return c.Img.GrayAt(x, y).Y < 100 }
	// Solid ink all the way: through the card, across the rim, and up the
	// margin above it to the top of the compass area.
	for _, y := range []int{100, 112, 125, 138, 200, 300, 400, 450} {
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
