package render

import (
	"image"
	"math"
	"testing"
)

func scaled(t *testing.T) *Canvas {
	t.Helper()
	c, err := NewScaledCanvas(600, 800, 1072)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestScaledCanvasGeometry(t *testing.T) {
	c := scaled(t)
	if b := c.Img.Bounds(); b.Dx() != 600 || b.Dy() != 800 {
		t.Fatalf("image is %v, want 600x800 device pixels", b)
	}
	b := c.Bounds()
	if b.Dx() != 1072 {
		t.Errorf("design width %d, want 1072", b.Dx())
	}
	// 800 / (600/1072) = 1429.3: the design height follows the screen's shape.
	if b.Dy() < 1427 || b.Dy() > 1431 {
		t.Errorf("design height %d, want about 1429", b.Dy())
	}
	if s := c.Scale(); math.Abs(s-600.0/1072) > 1e-9 {
		t.Errorf("scale %v", s)
	}
	// The whole design area is the whole device image.
	if d := c.Device(c.Bounds()); d != c.Img.Bounds() {
		t.Errorf("design bounds map to %v, want the whole image %v", d, c.Img.Bounds())
	}
	// A new canvas is white all over.
	for _, v := range c.Img.Pix {
		if v != White {
			t.Fatal("a new canvas should be white everywhere")
		}
	}
}

func TestOneToOneScaleIsExactlyTheOldBehaviour(t *testing.T) {
	// The scaling must not disturb a screen the layout was made on: drawing the
	// same scene on NewCanvas and on an explicit 1:1 scaled canvas must give the
	// same pixels, whatever the primitives.
	scene := func(c *Canvas) {
		c.FillRect(image.Rect(10, 20, 300, 90), Dark)
		c.HLine(0, 400, 150, 3, Black)
		c.VLine(200, 100, 500, 2, Mid)
		c.Line(20, 200, 380, 330, 5, Black)
		c.Ring(200, 300, 80, 6, Black)
		c.Disc(320, 120, 25, Black)
		c.FillPolygon([]image.Point{{50, 400}, {150, 420}, {100, 500}}, Black)
		c.Cog(60, 560, 30, Black, White)
		c.Text(20, 700, "Hello 123", 54, Bold, Left, Black)
		c.Text(380, 760, "Right", 40, Regular, Right, Black)
		c.Text(200, 820, "Centre", 40, Bold, Center, Black)
		c.TextRotated(300, 600, "W", 54, Bold, 1.0, Black)
	}
	a, _ := NewCanvas(400, 900)
	b, _ := NewScaledCanvas(400, 900, 400)
	scene(a)
	scene(b)
	for i := range a.Img.Pix {
		if a.Img.Pix[i] != b.Img.Pix[i] {
			t.Fatalf("pixel %d differs between NewCanvas and a 1:1 scaled canvas", i)
		}
	}
}

func TestRectanglesKeepSharingEdgesWhenScaled(t *testing.T) {
	// Boxes that tile the screen in design units must tile it on the device too,
	// with no gap and no overlap, or a grid would show cracks.
	const s = 600.0 / 1072
	for x := 0; x < 1072; x += 37 {
		left := ScaleRect(image.Rect(0, 0, x, 10), s)
		right := ScaleRect(image.Rect(x, 0, 1072, 10), s)
		if left.Max.X != right.Min.X {
			t.Fatalf("at x=%d the two halves meet at %d and %d", x, left.Max.X, right.Min.X)
		}
	}
}

func TestFillRectScalesAndAHairlineStaysVisible(t *testing.T) {
	c := scaled(t)
	c.FillRect(image.Rect(0, 0, 1072, 100), Black) // the top 100 design units
	rows := 0
	for y := 0; y < 800; y++ {
		if c.Img.GrayAt(300, y).Y == Black {
			rows++
		}
	}
	if rows < 55 || rows > 57 {
		t.Errorf("100 design units filled %d device rows, want about 56", rows)
	}
	// A 1-unit line is 0.56 of a pixel: it must still be drawn, as one pixel.
	h := scaled(t)
	h.HLine(0, 1072, 500, 1, Black)
	lines := 0
	for y := 0; y < 800; y++ {
		if h.Img.GrayAt(300, y).Y == Black {
			lines++
		}
	}
	if lines != 1 {
		t.Errorf("a hairline became %d device rows, want exactly 1", lines)
	}
	v := scaled(t)
	v.VLine(536, 0, 1429, 1, Black)
	cols := 0
	for x := 0; x < 600; x++ {
		if v.Img.GrayAt(x, 400).Y == Black {
			cols++
		}
	}
	if cols != 1 {
		t.Errorf("a vertical hairline became %d columns, want exactly 1", cols)
	}
}

func TestShapesLandWhereTheyShouldWhenScaled(t *testing.T) {
	c := scaled(t)
	s := c.Scale()
	// A disc at design (536, 300) is at device (300, 168).
	c.Disc(536, 300, 60, Black)
	cx, cy := int(math.Round(536*s)), int(math.Round(300*s))
	if c.Img.GrayAt(cx, cy).Y != Black {
		t.Errorf("the disc's middle (%d,%d) is not ink", cx, cy)
	}
	r := int(60 * s)
	if c.Img.GrayAt(cx+r+4, cy).Y != White || c.Img.GrayAt(cx-r-4, cy).Y != White {
		t.Error("ink outside the scaled disc's radius")
	}
	// A line from the top-left to the bottom-right corner touches both device corners.
	l := scaled(t)
	l.Line(0, 0, 1072, 1429, 4, Black)
	if l.Img.GrayAt(1, 1).Y != Black || l.Img.GrayAt(598, 798).Y != Black {
		t.Error("a corner-to-corner line should reach both device corners")
	}
	// A ring keeps its centre clear.
	g := scaled(t)
	g.Ring(536, 700, 400, 8, Black)
	if g.Img.GrayAt(300, int(700*s)).Y != White {
		t.Error("the ring's middle should be clear")
	}
	if g.Img.GrayAt(int(math.Round((536+400)*s)), int(math.Round(700*s))).Y > 100 {
		t.Error("the ring's right-hand edge is missing")
	}
	// A polygon.
	p := scaled(t)
	p.FillPolygon([]image.Point{{100, 100}, {500, 100}, {300, 500}}, Black)
	if p.Img.GrayAt(int(300*s), int(200*s)).Y != Black || p.Img.GrayAt(int(300*s), int(600*s)).Y != White {
		t.Error("the triangle fills the wrong place")
	}
}

func TestScaledTextWidthIsInDesignUnits(t *testing.T) {
	big, _ := NewCanvas(1072, 1448)
	small := scaled(t)
	for _, size := range []float64{34, 52, 114} {
		w1 := big.TextWidth("Closest 12.3", size, Bold)
		w2 := small.TextWidth("Closest 12.3", size, Bold)
		// The same words measured on the small screen give nearly the same
		// width in design units, since layout code relies on that.
		if d := math.Abs(float64(w1-w2)) / float64(w1); d > 0.04 {
			t.Errorf("size %v: width %d on the big canvas, %d on the small one (%.1f%% apart)", size, w1, w2, d*100)
		}
	}
}

func TestScaledTextIsDrawnSmallerButInTheSamePlace(t *testing.T) {
	big, _ := NewCanvas(1072, 1448)
	small := scaled(t)
	big.Text(200, 400, "Hello", 80, Bold, Left, Black)
	small.Text(200, 400, "Hello", 80, Bold, Left, Black)
	bb, sb := inkBox(big), inkBox(small)
	s := small.Scale()
	// The small ink box is the big one scaled, to within a pixel or two.
	want := ScaleRect(bb, s)
	for name, d := range map[string]int{
		"left": sb.Min.X - want.Min.X, "right": sb.Max.X - want.Max.X,
		"top": sb.Min.Y - want.Min.Y, "bottom": sb.Max.Y - want.Max.Y,
	} {
		if d < -2 || d > 2 {
			t.Errorf("the text's %s edge is %d px from where scaling the big one puts it (%v vs %v)", name, d, sb, want)
		}
	}
	// Right and centre alignment still hold on the small screen.
	r := scaled(t)
	r.Text(1000, 600, "Right", 60, Bold, Right, Black)
	if e := inkBox(r).Max.X; e < int(1000*s)-3 || e > int(1000*s)+1 {
		t.Errorf("right-aligned text ends at %d, want about %d", e, int(1000*s))
	}
	ce := scaled(t)
	ce.Text(536, 600, "Centre", 60, Bold, Center, Black)
	b2 := inkBox(ce)
	if mid := (b2.Min.X + b2.Max.X) / 2; mid < 296 || mid > 304 {
		t.Errorf("centred text is centred on %d, want about 300", mid)
	}
}

func TestScaledInkSizeAndRotatedTextAreInDesignUnits(t *testing.T) {
	big, _ := NewCanvas(1072, 1448)
	small := scaled(t)
	w1, h1 := big.InkSize("W", 54, Bold)
	w2, h2 := small.InkSize("W", 54, Bold)
	if abs(w1-w2) > 3 || abs(h1-h2) > 3 {
		t.Errorf("a W is %dx%d design units on the big canvas, %dx%d on the small", w1, h1, w2, h2)
	}
	// Rotated text lands where scaling its place puts it.
	small.TextRotated(536, 700, "W", 54, Bold, math.Pi/2, Black)
	box := inkBox(small)
	cx, cy := (box.Min.X+box.Max.X)/2, (box.Min.Y+box.Max.Y)/2
	if abs(cx-300) > 2 || abs(cy-int(700*small.Scale())) > 2 {
		t.Errorf("rotated W centred at (%d,%d), want about (300,%d)", cx, cy, int(700*small.Scale()))
	}
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}
