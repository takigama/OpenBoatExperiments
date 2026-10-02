// Package render is a small drawing toolkit on top of an 8-bit grayscale
// image: filled rectangles, lines and antialiased text using the embedded Go
// fonts. Everything is pure Go so it cross-compiles to any device with no C
// toolchain, and the same code draws the PNG previews on a PC.
package render

import (
	"image"
	"image/color"
	"image/draw"
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
