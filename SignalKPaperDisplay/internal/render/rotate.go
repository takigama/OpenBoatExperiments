package render

import (
	"image"
	"math"
)

// inkMask draws str upright and black on white into a scratch image, at devSize
// pixels (device pixels, not design units), and returns it with the rectangle
// that actually holds ink. Measuring the ink, not the font's nominal box, is
// what lets a caller put something an exact number of pixels away from the
// letters.
func (c *Canvas) inkMask(str string, devSize float64, w Weight) (*image.Gray, image.Rectangle) {
	scratch := &Canvas{faces: c.faces} // scale 0: drawn 1:1
	tw := scratch.TextWidth(str, devSize, w)
	pad := 6
	img := image.NewGray(image.Rect(0, 0, tw+2*pad, int(devSize*1.6)+2*pad))
	scratch.Img = img
	scratch.FillRect(img.Bounds(), White)
	scratch.Text(pad, pad+int(devSize*1.15), str, devSize, w, Left, Black)

	ink := image.Rectangle{Min: image.Pt(1<<30, 1<<30), Max: image.Pt(-1, -1)}
	for y := 0; y < img.Rect.Dy(); y++ {
		for x := 0; x < img.Rect.Dx(); x++ {
			if img.GrayAt(x, y).Y < 128 {
				ink.Min.X, ink.Min.Y = min(ink.Min.X, x), min(ink.Min.Y, y)
				ink.Max.X, ink.Max.Y = max(ink.Max.X, x+1), max(ink.Max.Y, y+1)
			}
		}
	}
	if ink.Max.X < 0 {
		return img, image.Rectangle{}
	}
	return img, ink
}

// design converts a length in device pixels to design units.
func (c *Canvas) design1(v int) int {
	if c.s() == 1 {
		return v
	}
	return int(math.Round(float64(v) / c.s()))
}

// InkSize is the width and height, in design units, of the pixels str is made
// of when drawn upright at this size - tighter than the font's own advance and
// line height.
func (c *Canvas) InkSize(str string, size float64, w Weight) (width, height int) {
	_, ink := c.inkMask(str, size*c.s(), w)
	return c.design1(ink.Dx()), c.design1(ink.Dy())
}

// TextRotated draws str with the middle of its ink at (cx, cy), turned angle
// radians clockwise about that point (0 is upright, pi/2 reads downwards, pi is
// upside-down). The letters are drawn upright once and resampled, so edges
// stay anti-aliased at any angle. It returns the ink's size before turning.
func (c *Canvas) TextRotated(cx, cy float64, str string, size float64, w Weight, angle float64, shade uint8) (inkW, inkH int) {
	sc := c.s()
	cx, cy = cx*sc, cy*sc // from here on, device pixels
	mask, ink := c.inkMask(str, size*sc, w)
	if ink.Empty() {
		return 0, 0
	}
	icx, icy := float64(ink.Min.X+ink.Max.X)/2, float64(ink.Min.Y+ink.Max.Y)/2
	sin, cos := math.Sincos(angle)
	reach := math.Hypot(float64(ink.Dx()), float64(ink.Dy()))/2 + 2

	sample := func(x, y float64) float64 { // ink coverage 0..1 at a point of the mask
		x0, y0 := math.Floor(x), math.Floor(y)
		fx, fy := x-x0, y-y0
		at := func(px, py int) float64 {
			if !image.Pt(px, py).In(mask.Rect) {
				return 0
			}
			return 1 - float64(mask.GrayAt(px, py).Y)/255
		}
		ix, iy := int(x0), int(y0)
		return at(ix, iy)*(1-fx)*(1-fy) + at(ix+1, iy)*fx*(1-fy) + at(ix, iy+1)*(1-fx)*fy + at(ix+1, iy+1)*fx*fy
	}
	for y := int(cy - reach); y <= int(cy+reach)+1; y++ {
		for x := int(cx - reach); x <= int(cx+reach)+1; x++ {
			dx, dy := float64(x)+0.5-cx, float64(y)+0.5-cy
			// Undo the turn to find where in the upright mask this pixel comes from.
			px := icx + dx*cos + dy*sin
			py := icy - dx*sin + dy*cos
			c.blend(x, y, shade, sample(px-0.5, py-0.5))
		}
	}
	return c.design1(ink.Dx()), c.design1(ink.Dy())
}
