package pages

import (
	"image"
	"image/color"
	"math"
	"testing"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func grayOf(v int) color.Gray { return color.Gray{Y: uint8(v)} }

// designH is the design height of a 600x800 screen: 800 / (600/1072).
const designH = 1429

// downsample shrinks a render to w x h by averaging, the reference a scaled
// render is compared against.
func downsample(src *image.Gray, w, h int) *image.Gray {
	dst := image.NewGray(image.Rect(0, 0, w, h))
	sx, sy := float64(src.Bounds().Dx())/float64(w), float64(src.Bounds().Dy())/float64(h)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			x0, x1 := int(float64(x)*sx), int(math.Ceil(float64(x+1)*sx))
			y0, y1 := int(float64(y)*sy), int(math.Ceil(float64(y+1)*sy))
			sum, n := 0, 0
			for yy := y0; yy < y1 && yy < src.Bounds().Dy(); yy++ {
				for xx := x0; xx < x1 && xx < src.Bounds().Dx(); xx++ {
					sum += int(src.GrayAt(xx, yy).Y)
					n++
				}
			}
			if n > 0 {
				dst.SetGray(x, y, grayOf(sum/n))
			}
		}
	}
	return dst
}

// blockDarkness is the average darkness (0 white .. 1 black) of each block of
// the picture, to compare two renders' structure without caring about
// single-pixel differences.
func blockDarkness(img *image.Gray, bw, bh int) []float64 {
	var out []float64
	for by := 0; by+bh <= img.Bounds().Dy(); by += bh {
		for bx := 0; bx+bw <= img.Bounds().Dx(); bx += bw {
			sum := 0
			for y := by; y < by+bh; y++ {
				for x := bx; x < bx+bw; x++ {
					sum += 255 - int(img.GrayAt(x, y).Y)
				}
			}
			out = append(out, float64(sum)/float64(bw*bh*255))
		}
	}
	return out
}

func correlation(a, b []float64) float64 {
	var ma, mb float64
	for i := range a {
		ma += a[i]
		mb += b[i]
	}
	ma, mb = ma/float64(len(a)), mb/float64(len(b))
	var cov, va, vb float64
	for i := range a {
		cov += (a[i] - ma) * (b[i] - mb)
		va += (a[i] - ma) * (a[i] - ma)
		vb += (b[i] - mb) * (b[i] - mb)
	}
	if va == 0 || vb == 0 {
		return 0
	}
	return cov / math.Sqrt(va*vb)
}

// fullBoat is a boat with data for every part of the compass and Nav pages.
func fullBoat() signalk.Snapshot {
	s := navBoat(2)
	own, _ := everything()
	s.Own.Extra, s.Own.Autopilot = own.Extra, own.Autopilot
	s.Own.AWA = signalk.Reading{V: 0.6, At: compassNow}
	s.Own.AWS = signalk.Reading{V: 6, At: compassNow}
	s.Own.TWS = signalk.Reading{V: 5, At: compassNow}
	s.Own.TWD = signalk.Reading{V: s.Own.Heading.V + 1.0, At: compassNow}
	s.Own.WaterTemp = signalk.Reading{V: 295, At: compassNow}
	s.Own.WPBearing = signalk.Reading{V: 0.9, At: compassNow}
	s.Own.WPDistance = signalk.Reading{V: 6000, At: compassNow}
	s.Own.Fuel = []signalk.Tank{{ID: "0", Level: signalk.Reading{V: 0.6, At: compassNow}}}
	return s
}

func TestEveryPageDrawsOnASmallerScreenLikeTheBigOneShrunk(t *testing.T) {
	const devW, devH = 600, 800
	env := Env{Units: units.Settings{Preset: units.PresetMetric}, Battery: &battery.Status{Percent: 80, Plugged: true}}
	snap := fullBoat()
	for _, p := range All() {
		big, _ := render.NewCanvas(1072, designH) // the same design area the small screen lays out in
		p.Draw(big, snap, compassNow, env)
		small, err := render.NewScaledCanvas(devW, devH, DesignWidth)
		if err != nil {
			t.Fatal(err)
		}
		p.Draw(small, snap, compassNow, env)
		ref := downsample(big.Img, devW, devH)

		corr := correlation(blockDarkness(small.Img, 20, 20), blockDarkness(ref, 20, 20))
		if corr < 0.93 {
			t.Errorf("%s: the %dx%d render matches the shrunk full-size one only to %.3f (want 0.93+): the layout drifted", p.ID, devW, devH, corr)
		}
		// And it is not a blank page, nor a black one.
		ink := inked(small, image.Rect(0, 0, devW, devH))
		if ink < 3000 || ink > devW*devH*3/4 {
			t.Errorf("%s: %d inked pixels on the small screen", p.ID, ink)
		}
	}
}

func TestSettingsScreensDrawOnASmallerScreen(t *testing.T) {
	const devW, devH = 600, 800
	views := []SettingsView{
		{Screen: SettingsRoot, MaxLevel: 24, Level: 6}, {Screen: SettingsPickPreset}, {Screen: SettingsBoxes},
		{Screen: SettingsPickBox, Page: 1}, {Screen: SettingsServer, Text: "10.0.0.76:3001"}, {Screen: SettingsLight, MaxLevel: 24, Level: 9},
	}
	for _, v := range views {
		big, _ := render.NewCanvas(1072, designH)
		Settings(big, v, units.Settings{Preset: units.PresetMetric}, false, nil, "10.0.0.76:3001")
		small, _ := render.NewScaledCanvas(devW, devH, DesignWidth)
		Settings(small, v, units.Settings{Preset: units.PresetMetric}, false, nil, "10.0.0.76:3001")
		ref := downsample(big.Img, devW, devH)
		a := blockDarkness(small.Img, 20, 20)
		b := blockDarkness(ref, 20, 20)
		if corr := correlation(a, b); corr < 0.9 {
			t.Errorf("settings screen %+v: only %.3f like the shrunk full-size one", v, corr)
		}
	}
}

func TestSmallScreenTextStaysReadable(t *testing.T) {
	// The smallest labels are 34 design units; on the 600-wide screen that is
	// 19 px. Check the text of the smallest label is still made of solid ink,
	// not a faint smear: some pixel in it is near black.
	small, _ := render.NewScaledCanvas(600, 800, DesignWidth)
	small.Text(28, 130, "WATER", 34, render.Bold, render.Left, render.Black)
	darkest := uint8(255)
	b := inkBounds(small)
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			darkest = min(darkest, small.Img.GrayAt(x, y).Y)
		}
	}
	if darkest > 40 {
		t.Errorf("the smallest label's darkest pixel is %d: it will be a smudge on the panel", darkest)
	}
	if b.Dy() < 10 {
		t.Errorf("the smallest label is only %d px tall", b.Dy())
	}
}

func inkBounds(c *render.Canvas) image.Rectangle {
	r := image.Rectangle{Min: image.Pt(1<<30, 1<<30), Max: image.Pt(-1, -1)}
	b := c.Img.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			if c.Img.GrayAt(x, y).Y < 200 {
				r.Min.X, r.Min.Y = min(r.Min.X, x), min(r.Min.Y, y)
				r.Max.X, r.Max.Y = max(r.Max.X, x+1), max(r.Max.Y, y+1)
			}
		}
	}
	return r
}

func TestHeartbeatImageOnASmallerScreenIsScaled(t *testing.T) {
	img, r := HeartbeatImage(600, true, false)
	want := render.ScaleRect(HeartbeatRect(DesignWidth), 600.0/1072)
	if r != want {
		t.Errorf("the dot's region is %v, want %v", r, want)
	}
	if img.Bounds().Dx() != want.Dx() || img.Bounds().Dy() != want.Dy() {
		t.Errorf("the dot's image is %v", img.Bounds())
	}
	if litPix(img) == 0 {
		t.Error("an on dot should have ink")
	}
	// Off draws nothing but its (white) background.
	off, _ := HeartbeatImage(600, false, false)
	if litPix(off) != 0 {
		t.Error("an off dot should be blank")
	}
}

func litPix(img *image.Gray) int {
	n := 0
	for y := img.Bounds().Min.Y; y < img.Bounds().Max.Y; y++ {
		for x := img.Bounds().Min.X; x < img.Bounds().Max.X; x++ {
			if img.GrayAt(x, y).Y < 128 {
				n++
			}
		}
	}
	return n
}
