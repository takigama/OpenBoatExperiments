package pages

import (
	"image"
	"math"
	"testing"
	"time"

	"signalkpaperdisplay/internal/signalk"
)

// moving is the base boat heading north at 3 m/s, with a course over ground
// of cog radians.
func moving(cog float64) signalk.Snapshot {
	s := base()
	s.Own.Heading = signalk.Reading{V: 0, At: compassNow}
	s.Own.SOG = signalk.Reading{V: 3, At: compassNow}
	s.Own.COG = signalk.Reading{V: cog, At: compassNow}
	return s
}

func withoutCOG(s signalk.Snapshot) signalk.Snapshot {
	s.Own.COG = signalk.Reading{}
	return s
}

func TestCOGLineAppearsOnlyWhenMeaningful(t *testing.T) {
	none := renderCompass(t, withoutCOG(moving(0)))

	if !differs(renderCompass(t, moving(math.Pi/2)), none) {
		t.Fatal("a live COG while moving should draw a line")
	}

	slow := moving(math.Pi / 2)
	slow.Own.SOG.V = 0.1
	if differs(renderCompass(t, slow), renderCompass(t, withoutCOG(slow))) {
		t.Error("below the minimum speed COG is noise and must not be drawn")
	}

	stale := moving(math.Pi / 2)
	stale.Own.COG.At = compassNow.Add(-time.Minute)
	if differs(renderCompass(t, stale), renderCompass(t, withoutCOG(stale))) {
		t.Error("a stale COG must not be drawn")
	}

	// The line is drawn relative to the heading, so without a live heading it
	// must not appear. (The COG *number* still does - see
	// TestCOGNumberSitsUnderTheHeadingWhileMoving - so compare only where the
	// line's crossbar would be: past the ring, on the starboard side.)
	noHeading := moving(math.Pi / 2)
	noHeading.Own.Heading.At = compassNow.Add(-time.Minute)
	const cx, cy = 536, 622
	r := 466.0
	bar := image.Rect(cx+int(r)+12, cy-24, cx+int(r)+50, cy+24)
	if inked(renderCompass(t, noHeading), bar) != inked(renderCompass(t, withoutCOG(noHeading)), bar) {
		t.Error("COG line is drawn relative to heading, so without a live heading it must not be drawn")
	}
}

func TestCOGLineFollowsTheCourseAndPassesOutsideTheRing(t *testing.T) {
	// cx=536, cy=622, r=466 for the 1072x1448 canvas.
	const cx, cy, r = 536, 622, 466

	// A course 90 degrees to starboard of the bow points straight right: the
	// line's crossbar sits beyond the ring, past r+3 (the ring's outer edge).
	east := renderCompass(t, moving(math.Pi/2))
	none := renderCompass(t, withoutCOG(moving(0)))
	beyond := image.Rect(cx+r+12, cy-24, cx+r+50, cy+24)
	if inked(east, beyond) <= inked(none, beyond) {
		t.Error("the COG line should reach past the compass ring")
	}

	// And it is NOT where a course to port would put it.
	port := renderCompass(t, moving(-math.Pi/2))
	if inked(port, beyond) > inked(none, beyond) {
		t.Error("a port course must not draw on the starboard side")
	}
	left := image.Rect(cx-r-50, cy-24, cx-r-12, cy+24)
	if inked(port, left) <= inked(none, left) {
		t.Error("a course to port should reach past the ring on the left")
	}
}

func TestCOGNumberSitsUnderTheHeadingWhileMoving(t *testing.T) {
	const cx, cy = 536, 622
	r := 466.0
	// Under the heading digits: below their baseline (cy+0.06r) down to where
	// the offset number ends (about cy+0.36r).
	under := image.Rect(cx-150, cy+int(0.14*r), cx+150, cy+int(0.38*r))

	live := renderCompass(t, moving(0.3))
	none := renderCompass(t, withoutCOG(moving(0)))
	if inked(live, under) <= inked(none, under) {
		t.Error("a live COG should show a number just under the heading")
	}

	// Same rules as the line: not when barely moving, not when stale.
	slow := moving(0.3)
	slow.Own.SOG.V = 0.1
	if inked(renderCompass(t, slow), under) != inked(renderCompass(t, withoutCOG(slow)), under) {
		t.Error("below the minimum speed the COG number must not be drawn")
	}
	stale := moving(0.3)
	stale.Own.COG.At = compassNow.Add(-time.Minute)
	if inked(renderCompass(t, stale), under) != inked(renderCompass(t, withoutCOG(stale)), under) {
		t.Error("a stale COG number must not be drawn")
	}

	// The readout is an offset from the heading, so with no live heading
	// there is nothing to measure it against and it must not be drawn.
	noHeading := moving(0.3)
	noHeading.Own.Heading.At = compassNow.Add(-time.Minute)
	if inked(renderCompass(t, noHeading), under) != inked(renderCompass(t, withoutCOG(noHeading)), under) {
		t.Error("without a live heading the COG offset has no reference and must not be drawn")
	}
}

func TestCOGStemOnlySpansTheOuterPartOfTheCard(t *testing.T) {
	const cx, cy = 536, 622
	r := 466.0 // a variable: int(0.7*r) on a constant won't compile
	// A course due east (heading north) is a horizontal line along y=cy.
	east := renderCompass(t, moving(math.Pi/2))
	none := renderCompass(t, withoutCOG(moving(0)))

	// Present in the outer part of the card...
	outerX := cx + int(0.7*r)
	outer := image.Rect(outerX, cy-4, outerX+30, cy+5)
	if inked(east, outer) <= inked(none, outer) {
		t.Error("the stem should be there in the outer part of the card")
	}
	// ...but absent nearer the middle, where it used to run.
	for _, frac := range []float64{0.30, 0.45, 0.55} {
		x := cx + int(frac*r)
		cell := image.Rect(x, cy-6, x+12, cy+7)
		if inked(east, cell) != inked(none, cell) {
			t.Errorf("the stem should not reach as far in as %.0f%% of the radius", frac*100)
		}
	}
}

func TestCOGCrossbarShowsEvenWhenTheStemIsHiddenUnderTheHeadingLine(t *testing.T) {
	// COG exactly equal to heading: the COG stem lies along the (much thinner)
	// heading line, so the stem and its crossbar are drawn over/around it. The
	// crossbar ends must stick out either side of the heading line, which is
	// now just 4px wide.
	const cx, cy, r = 536, 622, 466
	same := renderCompass(t, moving(0))
	none := renderCompass(t, withoutCOG(moving(0)))

	barY := cy - r - 34
	for _, dx := range []int{-16, -12, 12, 16} {
		cell := image.Rect(cx+dx-1, barY-3, cx+dx+2, barY+4)
		if inked(same, cell) <= inked(none, cell) {
			t.Errorf("the crossbar should be visible %dpx from the heading line when COG equals heading", dx)
		}
	}
}

func TestCOGLineStopsShortOfTheTopOfTheCompassArea(t *testing.T) {
	// A course the same as the heading lies along the bow line, so look at
	// a slightly different one: the line must end just outside the ring
	// (about y=122 at the top), well below the bow line's top (y=96).
	s := renderCompass(t, moving(0.06))
	none := renderCompass(t, withoutCOG(moving(0)))
	high := image.Rect(500, 96, 600, 108) // the very top of the compass area
	if inked(s, high) != inked(none, high) {
		t.Error("the COG line must not run as high as the heading line")
	}
}
