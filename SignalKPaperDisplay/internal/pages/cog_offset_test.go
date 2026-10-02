package pages

import (
	"image"
	"math"
	"testing"

	"signalkpaperdisplay/internal/render"
)

func rad(d float64) float64 { return d * math.Pi / 180 }

func TestCOGOffset(t *testing.T) {
	cases := []struct {
		name         string
		cog, heading float64 // degrees
		wantDeg      int
		wantDir      int
	}{
		// The two examples from the spec.
		{"course to port: COG 29, heading 39 -> <10", 29, 39, 10, -1},
		{"course to starboard: COG 29, heading 19 -> 10>", 29, 19, 10, 1},
		{"on the heading", 100, 100, 0, 0},
		{"rounds to zero, so no arrow", 100.3, 100, 0, 0},
		// Across north the short way round, not 357 degrees the long way.
		{"359 against a heading of 2 is 3 to port", 359, 2, 3, -1},
		{"2 against a heading of 359 is 3 to starboard", 2, 359, 3, 1},
		// The true angle is reported however large; it's the display that
		// switches to chevrons once it no longer fits two digits.
		{"well to starboard", 190, 40, 150, 1},
		{"well to port", 40, 190, 150, -1},
		{"exactly astern", 180, 0, 180, 1},
		{"just inside two digits", 99, 0, 99, 1},
		{"just outside two digits", 100, 0, 100, 1},
	}
	for _, c := range cases {
		deg, dir := cogOffset(rad(c.cog), rad(c.heading))
		if deg != c.wantDeg || dir != c.wantDir {
			t.Errorf("%s: got (%d, %d), want (%d, %d)", c.name, deg, dir, c.wantDeg, c.wantDir)
		}
	}
}

func TestCOGOffsetChevronSitsOnTheSideOfTheCourse(t *testing.T) {
	const cx, cy = 536, 622
	r := 466.0
	y0, y1 := cy+int(0.14*r), cy+int(0.38*r)
	left := image.Rect(cx-190, y0, cx-110, y1)
	right := image.Rect(cx+110, y0, cx+190, y1)

	none := renderCompass(t, withoutCOG(moving(0)))

	// Heading north, course 17 degrees east of it: the chevron is on the right.
	stbd := renderCompass(t, moving(rad(17)))
	if inked(stbd, right) <= inked(none, right) {
		t.Error("a course to starboard should put a chevron on the right of the number")
	}
	if inked(stbd, left) != inked(none, left) {
		t.Error("a course to starboard must leave the left of the number clear")
	}

	// And mirrored for a course to port.
	port := renderCompass(t, moving(rad(-17)))
	if inked(port, left) <= inked(none, left) {
		t.Error("a course to port should put a chevron on the left of the number")
	}
	if inked(port, right) != inked(none, right) {
		t.Error("a course to port must leave the right of the number clear")
	}

	// On the heading: just "00", no chevron either side.
	on := renderCompass(t, moving(0))
	if inked(on, left) != inked(none, left) || inked(on, right) != inked(none, right) {
		t.Error("with the course on the heading there should be no chevron")
	}
}

func TestOffsetsOver99BecomeThreeChevrons(t *testing.T) {
	const cx, cy = 536, 622
	r := 466.0
	y0, y1 := cy+int(0.14*r), cy+int(0.38*r)
	middle := image.Rect(cx-100, y0, cx+100, y1)
	left := image.Rect(cx-190, y0, cx-110, y1)
	right := image.Rect(cx+110, y0, cx+190, y1)

	none := renderCompass(t, withoutCOG(moving(0)))
	two := renderCompass(t, moving(rad(99)))   // still digits
	over := renderCompass(t, moving(rad(100))) // chevrons
	if !differs(two, over) {
		t.Fatal("99 and 100 degrees should render differently: digits, then chevrons")
	}

	// The chevrons take the place of the digits, so nothing sits beside them.
	if inked(over, middle) <= inked(none, middle) {
		t.Error("three chevrons should be drawn where the number goes")
	}
	if inked(over, left) != inked(none, left) || inked(over, right) != inked(none, right) {
		t.Error("the chevrons replace the number and its side arrow; nothing should sit to either side")
	}

	// And they point the way the course lies.
	port := renderCompass(t, moving(rad(-120)))
	stbd := renderCompass(t, moving(rad(120)))
	if !differs(port, stbd) {
		t.Error("chevrons to port and to starboard must be mirror images, not identical")
	}
}

// darkest is the lowest (blackest) pixel value in r.
func darkest(c *render.Canvas, r image.Rectangle) uint8 {
	d := uint8(255)
	for y := r.Min.Y; y < r.Max.Y; y++ {
		for x := r.Min.X; x < r.Max.X; x++ {
			if v := c.Img.GrayAt(x, y).Y; v < d {
				d = v
			}
		}
	}
	return d
}

// Anything redrawn in place has to be solid black: the fast waveform used
// for partial refreshes can't show grey, and a grey unit that moved because
// its number got wider simply vanished from the panel.
func TestUnitsAndStaleValuesAreSolidBlack(t *testing.T) {
	cell := image.Rect(0, 0, 536, 300)
	lower := image.Rect(0, 130, 536, 300) // below the label, where the value and unit are

	// An empty value leaves only the unit to draw, so any ink down there is it.
	c, err := render.NewCanvas(cell.Dx(), cell.Dy())
	if err != nil {
		t.Fatal(err)
	}
	drawMetric(c, cell, metric{label: "SPEED", unit: "km/h", value: "", ok: true}, 0)
	if got := darkest(c, lower); got > 10 {
		t.Errorf("the unit is drawn in grey %d; it must be solid black to survive the fast waveform", got)
	}

	// A stale value shows as dashes, and those must be black too, not greyed out.
	c, _ = render.NewCanvas(cell.Dx(), cell.Dy())
	drawMetric(c, cell, metric{label: "SPEED", unit: "", value: "9.9", ok: false}, 0)
	if got := darkest(c, lower); got > 10 {
		t.Errorf("a stale value is drawn in grey %d; it must be solid black", got)
	}
}
