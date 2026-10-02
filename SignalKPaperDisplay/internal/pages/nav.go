// Package pages holds the screens. A page is just a function that draws a
// SignalK snapshot onto a canvas - no I/O, no device knowledge - so each one
// can be previewed as a PNG and reused unchanged on every platform.
package pages

import (
	"fmt"
	"image"
	"math"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
)

// StaleAfter is how old a value can be before it's shown as unavailable.
// A frozen number that looks live is the worst failure on a boat.
const StaleAfter = 5 * time.Second

const headerH = 90

func degrees(rad float64) float64 {
	d := math.Mod(rad*180/math.Pi, 360)
	if d < 0 {
		d += 360
	}
	return d
}

// metric is one big labelled number.
type metric struct {
	label, unit string
	value       string
	ok          bool
}

func drawMetric(c *render.Canvas, r image.Rectangle, m metric) {
	shade := render.Black
	value := m.value
	if !m.ok {
		shade = render.Mid
		value = "--"
	}
	c.Text(r.Min.X+40, r.Min.Y+80, m.label, 52, render.Bold, render.Left, render.Dark)

	size := float64(r.Dy()) * 0.62
	baseline := r.Max.Y - int(float64(r.Dy())*0.2)

	// The unit sits immediately after the number and the pair is centred
	// together, so it reads as one value ("4.9 kn"), not a number with a
	// stray label on the far edge. Shrink the whole thing until it fits the
	// cell - half-width cells are much narrower than the full-width ones.
	var unitSize, wv, wu float64
	var gap, unitBase int
	for ; ; size *= 0.95 {
		unitSize, gap, unitBase = size*0.30, int(size*0.08), baseline
		if m.unit == "°" {
			// A degree sign belongs at the top of the digits, tucked up
			// against them, not on the baseline.
			unitSize, gap, unitBase = size*0.6, int(size*0.02), baseline-int(size*0.27)
		}
		wv = float64(c.TextWidth(value, size, render.Bold))
		wu = float64(c.TextWidth(m.unit, unitSize, render.Bold))
		if wv+float64(gap)+wu <= float64(r.Dx()-80) || size < 40 {
			break
		}
	}
	wvi := int(wv)
	x := r.Min.X + (r.Dx()-(wvi+gap+int(wu)))/2
	c.Text(x, baseline, value, size, render.Bold, render.Left, shade)
	c.Text(x+wvi+gap, unitBase, m.unit, unitSize, render.Bold, render.Left, render.Dark)
}

// Header draws the top bar: the page title, and a loud inverted banner
// whenever the server link is down or data has stopped arriving.
func Header(c *render.Canvas, title string, s signalk.Snapshot, now time.Time) {
	b := c.Bounds()
	lost := !s.Connected || !s.LastMessage.After(now.Add(-StaleAfter))
	if lost {
		c.FillRect(image.Rect(0, 0, b.Dx(), headerH), render.Black)
		c.Text(b.Dx()/2, 64, "NO DATA", 60, render.Bold, render.Center, render.White)
	} else {
		c.Text(40, 64, title, 56, render.Bold, render.Left, render.Black)
	}
	c.HLine(0, b.Dx(), headerH, 4, render.Black)
}

// Nav shows speed, heading and depth, in the user's chosen units.
func Nav(c *render.Canvas, s signalk.Snapshot, now time.Time, e Env) {
	Header(c, "NAV", s, now)
	b := c.Bounds()
	own := s.Own

	sogVal, sogUnit := e.Units.Format("sog", own.SOG.V)
	depthVal, depthUnit := e.Units.Format("depth", own.Depth.V)
	metrics := []metric{
		{label: "SPEED OVER GROUND", unit: sogUnit, ok: own.SOG.Fresh(now, StaleAfter), value: sogVal},
		{label: "HEADING", unit: "°", ok: own.Heading.Fresh(now, StaleAfter),
			value: fmt.Sprintf("%03.0f", degrees(own.Heading.V))},
		{label: "DEPTH", unit: depthUnit, ok: own.Depth.Fresh(now, StaleAfter), value: depthVal},
	}

	top := headerH + 4
	cell := (b.Dy() - top) / len(metrics)
	for i, m := range metrics {
		y0 := top + i*cell
		drawMetric(c, image.Rect(0, y0, b.Dx(), y0+cell), m)
		if i < len(metrics)-1 {
			c.HLine(0, b.Dx(), y0+cell-2, 3, render.Mid)
		}
	}
}
