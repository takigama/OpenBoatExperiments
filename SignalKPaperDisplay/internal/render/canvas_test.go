package render

import (
	"image"
	"testing"
)

func TestRingDrawsOnlyTheBand(t *testing.T) {
	c, err := NewCanvas(240, 240)
	if err != nil {
		t.Fatal(err)
	}
	c.Ring(120, 120, 80, 6, Black)

	dark := func(x, y int) bool { return c.Img.GrayAt(x, y).Y < 128 }

	// On the circle, in all four directions and on a diagonal.
	for _, p := range []image.Point{{200, 120}, {40, 120}, {120, 40}, {120, 200}, {177, 177}} {
		if !dark(p.X, p.Y) {
			t.Errorf("pixel %v on the ring should be dark, got %d", p, c.Img.GrayAt(p.X, p.Y).Y)
		}
	}
	// Centre, just inside and just outside must stay white.
	for _, p := range []image.Point{{120, 120}, {120, 60}, {135, 150}, {120, 215}, {5, 5}} {
		if c.Img.GrayAt(p.X, p.Y).Y != White {
			t.Errorf("pixel %v is off the ring and should be untouched, got %d", p, c.Img.GrayAt(p.X, p.Y).Y)
		}
	}
}

func TestRingWithNoHoleStillFills(t *testing.T) {
	// A ring thicker than its radius has no empty middle; the optimisation
	// that skips the hole must not skip anything it shouldn't.
	c, _ := NewCanvas(60, 60)
	c.Ring(30, 30, 4, 20, Black)
	if c.Img.GrayAt(30, 30).Y > 128 {
		t.Error("the middle of a very thick ring should be filled")
	}
}
