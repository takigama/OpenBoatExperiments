// Package render is a small drawing toolkit on top of an 8-bit grayscale
// image: filled rectangles, lines and antialiased text using the embedded Go
// fonts. Everything is pure Go so it cross-compiles to any device with no C
// toolchain, and the same code draws the PNG previews on a PC.
package render

import (
	"image"
	"image/color"
	"math"
	"sort"
	"sync"

	"golang.org/x/image/font"
	"golang.org/x/image/font/gofont/gobold"
	"golang.org/x/image/font/gofont/goregular"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
)

// Shades. E-ink shows ~16 levels; these are well separated.
const (
	Black uint8 = 0
	Dark  uint8 = 70
	Mid   uint8 = 150
	White uint8 = 255
)

type Weight int

const (
	Regular Weight = iota
	Bold
)

type Align int

const (
	Left Align = iota
	Center
	Right
)

var (
	fontsOnce sync.Once
	fonts     [2]*opentype.Font
	fontsErr  error
)

func loadFonts() error {
	fontsOnce.Do(func() {
		for i, ttf := range [][]byte{goregular.TTF, gobold.TTF} {
			fonts[i], fontsErr = opentype.Parse(ttf)
			if fontsErr != nil {
				return
			}
		}
	})
	return fontsErr
}

type faceKey struct {
	w    Weight
	size float64
}

// Canvas draws in "design" units - the coordinates the pages are laid out in -
// onto an image of the real device's size. The two are the same size unless a
// canvas was made with NewScaledCanvas, which is how one layout fits every
// screen: a 600-pixel-wide Kindle draws the same page as a 1072-wide one, at
// 56% size. Positions, sizes, line weights and text all scale; callers never
// see device pixels (except through Img, which is the finished picture).
type Canvas struct {
	Img    *image.Gray
	faces  map[faceKey]font.Face
	scale  float64         // device pixels per design unit; 0 means 1
	design image.Rectangle // the drawing area in design units (when scale != 0)
}

// NewCanvas returns a white canvas of the given size, drawn 1:1.
func NewCanvas(w, h int) (*Canvas, error) { return NewScaledCanvas(w, h, w) }

// NewScaledCanvas returns a white canvas whose image is devW x devH pixels but
// which is drawn on as if it were designW units wide. The design height is
// whatever fits, so a screen with a slightly different shape just gets a
// slightly different height.
func NewScaledCanvas(devW, devH, designW int) (*Canvas, error) {
	if err := loadFonts(); err != nil {
		return nil, err
	}
	s := float64(devW) / float64(designW)
	c := &Canvas{
		Img:    image.NewGray(image.Rect(0, 0, devW, devH)),
		faces:  map[faceKey]font.Face{},
		scale:  s,
		design: image.Rect(0, 0, designW, int(math.Round(float64(devH)/s))),
	}
	c.FillRect(c.Bounds(), White)
	return c, nil
}

// s is the scale, with the zero value (a Canvas built by hand) meaning 1.
func (c *Canvas) s() float64 {
	if c.scale == 0 {
		return 1
	}
	return c.scale
}

// Scale is the number of device pixels to a design unit.
func (c *Canvas) Scale() float64 { return c.s() }

// Bounds is the drawing area in design units - what pages lay themselves out in.
func (c *Canvas) Bounds() image.Rectangle {
	if c.scale == 0 {
		return c.Img.Bounds()
	}
	return c.design
}

// ScaleRect converts a rectangle between units, rounding each edge, so
// rectangles that share an edge still share it afterwards.
func ScaleRect(r image.Rectangle, s float64) image.Rectangle {
	return image.Rect(int(math.Round(float64(r.Min.X)*s)), int(math.Round(float64(r.Min.Y)*s)),
		int(math.Round(float64(r.Max.X)*s)), int(math.Round(float64(r.Max.Y)*s)))
}

// Device converts a rectangle in design units to device pixels.
func (c *Canvas) Device(r image.Rectangle) image.Rectangle { return ScaleRect(r, c.s()) }

// FillRect fills r (in design units, clipped to the canvas) with a solid
// shade. It writes the pixels directly: image/draw has no fast path for
// filling a grayscale image, and its generic one made this - called for every
// rectangle, line segment and the background - about four fifths of the time
// spent drawing a page. A rectangle that scales to nothing still gets a pixel,
// so a hairline stays visible on a small screen.
func (c *Canvas) FillRect(r image.Rectangle, shade uint8) {
	d := c.Device(r)
	if !r.Empty() {
		if d.Dx() == 0 {
			d.Max.X = d.Min.X + 1
		}
		if d.Dy() == 0 {
			d.Max.Y = d.Min.Y + 1
		}
	}
	c.fillDev(d, shade)
}

// fillDev is FillRect on device pixels.
func (c *Canvas) fillDev(r image.Rectangle, shade uint8) {
	r = r.Intersect(c.Img.Bounds())
	if r.Empty() {
		return
	}
	for y := r.Min.Y; y < r.Max.Y; y++ {
		row := c.Img.Pix[c.Img.PixOffset(r.Min.X, y):][:r.Dx()]
		for i := range row {
			row[i] = shade
		}
	}
}

// HLine and VLine draw lines of the given thickness.
func (c *Canvas) HLine(x0, x1, y, thick int, shade uint8) {
	c.FillRect(image.Rect(x0, y, x1, y+thick), shade)
}

func (c *Canvas) VLine(x, y0, y1, thick int, shade uint8) {
	c.FillRect(image.Rect(x, y0, x+thick, y1), shade)
}

// Line draws a straight line of the given thickness between two points.
func (c *Canvas) Line(x0, y0, x1, y1 float64, thick int, shade uint8) {
	s := c.s()
	x0, y0, x1, y1 = x0*s, y0*s, x1*s, y1*s
	t := max(1, int(math.Round(float64(thick)*s)))
	dx, dy := x1-x0, y1-y0
	n := int(math.Max(math.Abs(dx), math.Abs(dy))) + 1
	half := t / 2
	for i := 0; i <= n; i++ {
		f := float64(i) / float64(n)
		x := int(math.Round(x0 + dx*f))
		y := int(math.Round(y0 + dy*f))
		c.fillDev(image.Rect(x-half, y-half, x-half+t, y-half+t), shade)
	}
}

// blend mixes shade into one pixel with the given coverage (0..1).
func (c *Canvas) blend(x, y int, shade uint8, cover float64) {
	if cover <= 0 || !image.Pt(x, y).In(c.Img.Bounds()) {
		return
	}
	if cover > 1 {
		cover = 1
	}
	old := float64(c.Img.GrayAt(x, y).Y)
	c.Img.SetGray(x, y, color.Gray{Y: uint8(math.Round(old*(1-cover) + float64(shade)*cover))})
}

// Ring draws an antialiased circle outline centred on (cx, cy).
func (c *Canvas) Ring(cx, cy, r float64, thick float64, shade uint8) {
	s := c.s()
	c.ringDev(cx*s, cy*s, r*s, math.Max(thick*s, 1), shade)
}

func (c *Canvas) ringDev(cx, cy, r float64, thick float64, shade uint8) {
	// Only a thin band around the circle can be touched, so for each row
	// work out where that band starts and ends instead of testing every
	// pixel of the bounding box - on a Kindle the full scan cost ~300ms.
	outer := r + thick/2 + 2
	inner := math.Max(0, r-thick/2-2)
	for y := int(cy - outer); y <= int(cy+outer); y++ {
		dy := float64(y) - cy
		if math.Abs(dy) > outer {
			continue
		}
		xo := int(math.Sqrt(outer*outer-dy*dy)) + 1
		xi := 0 // the band is a single run through the middle on rows beyond the hole
		if math.Abs(dy) < inner {
			xi = int(math.Sqrt(inner*inner-dy*dy)) - 1
		}
		for x := int(cx) - xo; x <= int(cx)+xo; x++ {
			if xi > 0 && x > int(cx)-xi && x < int(cx)+xi {
				continue // inside the hole
			}
			d := math.Hypot(float64(x)-cx, dy)
			c.blend(x, y, shade, thick/2-math.Abs(d-r)+0.5)
		}
	}
}

// Disc draws an antialiased filled circle.
func (c *Canvas) Disc(cx, cy, r float64, shade uint8) {
	s := c.s()
	c.discDev(cx*s, cy*s, r*s, shade)
}

func (c *Canvas) discDev(cx, cy, r float64, shade uint8) {
	for y := int(cy - r - 1); y <= int(cy+r+1); y++ {
		dy := float64(y) - cy
		if math.Abs(dy) > r+1 {
			continue
		}
		half := int(math.Sqrt(math.Max(0, (r+1)*(r+1)-dy*dy))) + 1
		for x := int(cx) - half; x <= int(cx)+half; x++ {
			c.blend(x, y, shade, r-math.Hypot(float64(x)-cx, dy)+0.5)
		}
	}
}

// Cog draws a gear icon of outer radius r: a body, eight teeth and a hub
// hole cut in the background shade.
func (c *Canvas) Cog(cx, cy, r float64, shade, bg uint8) {
	c.Disc(cx, cy, r*0.72, shade)
	for i := 0; i < 8; i++ {
		a := float64(i) * math.Pi / 4
		ux, uy := math.Cos(a), math.Sin(a)
		vx, vy := -uy, ux
		pt := func(along, across float64) image.Point {
			return image.Pt(int(math.Round(cx+ux*along+vx*across)), int(math.Round(cy+uy*along+vy*across)))
		}
		c.FillPolygon([]image.Point{
			pt(r*0.6, r*0.22), pt(r, r*0.15), pt(r, -r*0.15), pt(r*0.6, -r*0.22),
		}, shade)
	}
	c.Disc(cx, cy, r*0.3, bg)
}

// FillPolygon fills a convex or simple polygon (points in design units) using
// an even-odd scanline.
func (c *Canvas) FillPolygon(pts []image.Point, shade uint8) {
	sc := c.s()
	dev := make([]image.Point, len(pts))
	for i, p := range pts {
		dev[i] = image.Pt(int(math.Round(float64(p.X)*sc)), int(math.Round(float64(p.Y)*sc)))
	}
	pts = dev
	minY, maxY := pts[0].Y, pts[0].Y
	for _, p := range pts {
		minY, maxY = min(minY, p.Y), max(maxY, p.Y)
	}
	for y := minY; y <= maxY; y++ {
		var xs []int
		for i := range pts {
			a, b := pts[i], pts[(i+1)%len(pts)]
			if (a.Y <= y && b.Y > y) || (b.Y <= y && a.Y > y) {
				t := float64(y-a.Y) / float64(b.Y-a.Y)
				xs = append(xs, a.X+int(math.Round(t*float64(b.X-a.X))))
			}
		}
		sort.Ints(xs)
		for i := 0; i+1 < len(xs); i += 2 {
			c.fillDev(image.Rect(xs[i], y, xs[i+1]+1, y+1), shade)
		}
	}
}

func (c *Canvas) face(size float64, w Weight) font.Face {
	k := faceKey{w, size}
	if f, ok := c.faces[k]; ok {
		return f
	}
	// 72 DPI makes Size mean pixels, which is how layout code thinks.
	f, err := opentype.NewFace(fonts[w], &opentype.FaceOptions{Size: size, DPI: 72, Hinting: font.HintingFull})
	if err != nil {
		panic(err) // only possible with invalid sizes - a programming error
	}
	c.faces[k] = f
	return f
}

// TextWidth is the rendered width of str in design units - what layout code
// needs to place things beside it.
func (c *Canvas) TextWidth(str string, size float64, w Weight) int {
	sc := c.s()
	d := font.Drawer{Face: c.face(size*sc, w)}
	px := d.MeasureString(str).Round() // device pixels
	if sc == 1 {
		return px
	}
	return int(math.Round(float64(px) / sc))
}

// Text draws str with its baseline at y, aligned relative to x (design units;
// size is the font size in design units too).
//
// The glyphs are blended into the canvas here rather than by font.Drawer,
// whose image/draw route goes pixel by pixel through generic interfaces; this
// is the same drawing, an order of magnitude cheaper on the Kindle's CPU.
func (c *Canvas) Text(x, baseline int, str string, size float64, w Weight, a Align, shade uint8) {
	sc := c.s()
	face := c.face(size*sc, w)
	d := font.Drawer{Face: face}
	width := d.MeasureString(str).Round()
	xd, yd := int(math.Round(float64(x)*sc)), int(math.Round(float64(baseline)*sc))
	switch a {
	case Center:
		xd -= width / 2
	case Right:
		xd -= width
	}
	dot := fixed.P(xd, yd)
	prev := rune(-1)
	for _, r := range str {
		if prev >= 0 {
			dot.X += face.Kern(prev, r)
		}
		dr, mask, maskp, advance, ok := face.Glyph(dot, r)
		if ok {
			c.blendMask(dr, mask, maskp, shade)
		}
		dot.X += advance
		prev = r
	}
}

// blendMask paints shade through an alpha mask: the pixel at dr.Min takes the
// mask value at maskp, and so on across dr.
func (c *Canvas) blendMask(dr image.Rectangle, mask image.Image, maskp image.Point, shade uint8) {
	m, ok := mask.(*image.Alpha)
	if !ok {
		return // the fonts here only produce alpha masks
	}
	clipped := dr.Intersect(c.Img.Bounds())
	if clipped.Empty() {
		return
	}
	maskp = maskp.Add(clipped.Min.Sub(dr.Min))
	sh := uint32(shade)
	for y := 0; y < clipped.Dy(); y++ {
		dst := c.Img.Pix[c.Img.PixOffset(clipped.Min.X, clipped.Min.Y+y):][:clipped.Dx()]
		src := m.Pix[m.PixOffset(maskp.X, maskp.Y+y):][:clipped.Dx()]
		for i, a := range src {
			switch a {
			case 0:
			case 255:
				dst[i] = shade
			default:
				ia := uint32(a)
				dst[i] = uint8((uint32(dst[i])*(255-ia) + sh*ia + 127) / 255)
			}
		}
	}
}
