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

type Canvas struct {
	Img   *image.Gray
	faces map[faceKey]font.Face
}

// NewCanvas returns a white canvas of the given size.
func NewCanvas(w, h int) (*Canvas, error) {
	if err := loadFonts(); err != nil {
		return nil, err
	}
	c := &Canvas{Img: image.NewGray(image.Rect(0, 0, w, h)), faces: map[faceKey]font.Face{}}
	c.FillRect(c.Img.Bounds(), White)
	return c, nil
}

func (c *Canvas) Bounds() image.Rectangle { return c.Img.Bounds() }

// FillRect fills r (clipped to the canvas) with a solid shade. It writes the
// pixels directly: image/draw has no fast path for filling a grayscale image,
// and its generic one made this - called for every rectangle, line segment and
// the background - about four fifths of the time spent drawing a page.
func (c *Canvas) FillRect(r image.Rectangle, shade uint8) {
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
	dx, dy := x1-x0, y1-y0
	n := int(math.Max(math.Abs(dx), math.Abs(dy))) + 1
	half := thick / 2
	for i := 0; i <= n; i++ {
		t := float64(i) / float64(n)
		x := int(math.Round(x0 + dx*t))
		y := int(math.Round(y0 + dy*t))
		c.FillRect(image.Rect(x-half, y-half, x-half+thick, y-half+thick), shade)
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

// FillPolygon fills a convex or simple polygon using an even-odd scanline.
func (c *Canvas) FillPolygon(pts []image.Point, shade uint8) {
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
			c.HLine(xs[i], xs[i+1]+1, y, 1, shade)
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

// TextWidth is the rendered width of s in pixels.
func (c *Canvas) TextWidth(s string, size float64, w Weight) int {
	d := font.Drawer{Face: c.face(size, w)}
	return d.MeasureString(s).Round()
}

// Text draws s with its baseline at y, aligned relative to x.
//
// The glyphs are blended into the canvas here rather than by font.Drawer,
// whose image/draw route goes pixel by pixel through generic interfaces; this
// is the same drawing, an order of magnitude cheaper on the Kindle's CPU.
func (c *Canvas) Text(x, baseline int, s string, size float64, w Weight, a Align, shade uint8) {
	face := c.face(size, w)
	d := font.Drawer{Face: face}
	width := d.MeasureString(s).Round()
	switch a {
	case Center:
		x -= width / 2
	case Right:
		x -= width
	}
	dot := fixed.P(x, baseline)
	prev := rune(-1)
	for _, r := range s {
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
