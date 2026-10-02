package render

import (
	"image"
	"math"
	"testing"
)

func inkBox(c *Canvas) image.Rectangle {
	r := image.Rectangle{Min: image.Pt(1<<30, 1<<30), Max: image.Pt(-1, -1)}
	b := c.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			if c.Img.GrayAt(x, y).Y < 128 {
				r.Min.X, r.Min.Y = min(r.Min.X, x), min(r.Min.Y, y)
				r.Max.X, r.Max.Y = max(r.Max.X, x+1), max(r.Max.Y, y+1)
			}
		}
	}
	return r
}

func inkCount(c *Canvas) int {
	n := 0
	for _, v := range c.Img.Pix {
		if v < 128 {
			n++
		}
	}
	return n
}

func TestInkSizeIsTighterThanTheAdvance(t *testing.T) {
	c, _ := NewCanvas(10, 10)
	w, h := c.InkSize("W", 54, Bold)
	if w <= 0 || h <= 0 {
		t.Fatalf("ink size %dx%d", w, h)
	}
	// A capital W: wider than tall, and its height is about a cap height (0.7 em).
	if w < h || float64(h) < 0.6*54 || float64(h) > 0.85*54 {
		t.Errorf("ink %dx%d does not look like a 54px capital W", w, h)
	}
	if adv := c.TextWidth("W", 54, Bold); w > adv {
		t.Errorf("ink %d wider than the advance %d", w, adv)
	}
	if w0, h0 := c.InkSize("", 54, Bold); w0 != 0 || h0 != 0 {
		t.Errorf("empty text has ink %dx%d", w0, h0)
	}
}

func TestTextRotatedTurnsAboutTheMiddleOfTheInk(t *testing.T) {
	draw := func(angle float64) (*Canvas, int, int) {
		c, _ := NewCanvas(200, 200)
		w, h := c.TextRotated(100, 100, "W", 54, Bold, angle, Black)
		return c, w, h
	}
	up, w, h := draw(0)
	box0 := inkBox(up)
	if d := box0.Dx() - w; d < -2 || d > 2 {
		t.Errorf("upright ink is %d wide, reported %d", box0.Dx(), w)
	}
	// Centred on the point it turns about.
	if cx := (box0.Min.X + box0.Max.X) / 2; cx < 98 || cx > 102 {
		t.Errorf("upright ink centred at x=%d, want 100", cx)
	}
	if cy := (box0.Min.Y + box0.Max.Y) / 2; cy < 98 || cy > 102 {
		t.Errorf("upright ink centred at y=%d, want 100", cy)
	}

	// A quarter turn swaps width and height, and stays centred.
	quarter, _, _ := draw(math.Pi / 2)
	b := inkBox(quarter)
	if dw, dh := b.Dx()-h, b.Dy()-w; dw < -3 || dw > 3 || dh < -3 || dh > 3 {
		t.Errorf("quarter turn is %dx%d, want about %dx%d", b.Dx(), b.Dy(), h, w)
	}
	if cx := (b.Min.X + b.Max.X) / 2; cx < 98 || cx > 102 {
		t.Errorf("quarter turn centred at x=%d", cx)
	}

	// Half a turn: the same size, and the letter really is upside-down (a W
	// has its two points at the top; turned over, they are at the bottom).
	half, _, _ := draw(math.Pi)
	bh := inkBox(half)
	if d := bh.Dx() - box0.Dx(); d < -2 || d > 2 {
		t.Errorf("half turn width %d vs %d", bh.Dx(), box0.Dx())
	}
	topHeavy := func(c *Canvas, r image.Rectangle) int { // ink in the top fifth minus the bottom fifth
		fifth := r.Dy() / 5
		top, bottom := 0, 0
		for y := r.Min.Y; y < r.Min.Y+fifth; y++ {
			for x := r.Min.X; x < r.Max.X; x++ {
				if c.Img.GrayAt(x, y).Y < 128 {
					top++
				}
			}
		}
		for y := r.Max.Y - fifth; y < r.Max.Y; y++ {
			for x := r.Min.X; x < r.Max.X; x++ {
				if c.Img.GrayAt(x, y).Y < 128 {
					bottom++
				}
			}
		}
		return top - bottom
	}
	if a, b := topHeavy(up, box0), topHeavy(half, bh); (a > 0) == (b > 0) {
		t.Errorf("turning it over did not flip the letter: %d then %d", a, b)
	}
	// Turning conserves the ink, give or take resampling.
	if a, b := inkCount(up), inkCount(half); math.Abs(float64(a-b)) > 0.06*float64(a) {
		t.Errorf("ink %d upright, %d turned", a, b)
	}
	if a, b := inkCount(up), inkCount(quarter); math.Abs(float64(a-b)) > 0.08*float64(a) {
		t.Errorf("ink %d upright, %d at a quarter turn", a, b)
	}
}

func TestTextRotatedStaysInsideTheCanvas(t *testing.T) {
	// Drawn half off the edge it must clip, not panic.
	c, _ := NewCanvas(60, 60)
	for _, a := range []float64{0, 1, 2.5, math.Pi} {
		c.TextRotated(0, 0, "W", 54, Bold, a, Black)
		c.TextRotated(60, 60, "W", 54, Bold, a, Black)
		c.TextRotated(-500, 30, "W", 54, Bold, a, Black)
	}
}
