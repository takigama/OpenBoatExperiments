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

	noHeading := moving(math.Pi / 2)
	noHeading.Own.Heading.At = compassNow.Add(-time.Minute)
	if differs(renderCompass(t, noHeading), renderCompass(t, withoutCOG(noHeading))) {
		t.Error("COG is drawn relative to heading, so without a live heading it must not be drawn")
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
