// Package render is a small drawing toolkit on top of an 8-bit grayscale
// image: filled rectangles, lines and antialiased text using the embedded Go
// fonts. Everything is pure Go so it cross-compiles to any device with no C
// toolchain, and the same code draws the PNG previews on a PC.
package render

import (
	"image"
	"image/color"
	"image/draw"
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

func (c *Canvas) FillRect(r image.Rectangle, shade uint8) {
	draw.Draw(c.Img, r, image.NewUniform(color.Gray{Y: shade}), image.Point{}, draw.Src)
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
	reach := int(r + thick + 2)
	for y := int(cy) - reach; y <= int(cy)+reach; y++ {
		for x := int(cx) - reach; x <= int(cx)+reach; x++ {
			d := math.Hypot(float64(x)-cx, float64(y)-cy)
			c.blend(x, y, shade, thick/2-math.Abs(d-r)+0.5)
		}
	}
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
func (c *Canvas) Text(x, baseline int, s string, size float64, w Weight, a Align, shade uint8) {
	d := font.Drawer{
		Dst:  c.Img,
		Src:  image.NewUniform(color.Gray{Y: shade}),
		Face: c.face(size, w),
	}
	width := d.MeasureString(s).Round()
	switch a {
	case Center:
		x -= width / 2
	case Right:
		x -= width
	}
	d.Dot = fixed.P(x, baseline)
	d.DrawString(s)
}
