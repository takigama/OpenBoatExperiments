// Package pages holds the screens. A page is just a function that draws a
// SignalK snapshot onto a canvas - no I/O, no device knowledge - so each one
// can be previewed as a PNG and reused unchanged on every platform.
package pages

import (
	"image"
	"math"
	"strings"
	"time"

	"signalkpaperdisplay/internal/ais"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
)

// StaleAfter is how old a value can be before it's shown as unavailable.
// A frozen number that looks live is the worst failure on a boat.
const StaleAfter = 5 * time.Second

const headerH = 90

// HeaderBand is the strip across the top of the screen, header and rule, that
// every page and the settings screens draw the same way.
func HeaderBand(width int) image.Rectangle { return image.Rect(0, 0, width, headerH+4) }

// The settings cog sits at the left of every page's header, with the title
// beside it.
const (
	cogX, cogY, cogR = 58.0, 46.0, 30.0
	titleX           = 112
)

// CogRect is the area that opens settings. It's deliberately bigger than
// the icon - a fingertip on a moving boat isn't precise.
var CogRect = image.Rect(0, 0, 170, headerH+10)

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
	// liveLabel draws the label in solid black. Static labels are grey, which is
	// fine because they are only drawn on a full refresh; a label that changes
	// while the screen is up (the speed box's SOG/STW/VMG) is redrawn in place
	// under the fast waveform, which can't show grey.
	liveLabel bool
}

// A note on shades, since it caused a real bug: anything that is redrawn in
// place - values, their units, gauges - is drawn in solid black, never grey.
// Partial refreshes use the fast DU waveform, which can only show black or
// white, and the panel only redraws pixels that changed. A grey unit that
// moved because its number got wider simply vanished. Static labels can stay
// grey: they never change, so they keep the grey from the last full refresh.
func drawMetric(c *render.Canvas, r image.Rectangle, m metric, maxSize float64) {
	value := m.value
	if !m.ok {
		value = "--" // the dashes say "stale"; greying them out would just make them vanish
	}
	labelShade := render.Dark
	if m.liveLabel {
		labelShade = render.Black
	}
	c.Text(r.Min.X+40, r.Min.Y+80, m.label, 52, render.Bold, render.Left, labelShade)

	baseline := r.Max.Y - int(float64(r.Dy())*0.2)
	size, unitSize, gap, wv, wu := fitMetric(c, r, value, m.unit, maxSize)

	// The unit sits immediately after the number and the pair is centred
	// together, so it reads as one value ("4.9 kn"), not a number with a
	// stray label on the far edge.
	x := r.Min.X + (r.Dx()-(wv+gap+wu))/2
	c.Text(x, baseline, value, size, render.Bold, render.Left, render.Black)
	c.Text(x+wv+gap, baseline, m.unit, unitSize, render.Bold, render.Left, render.Black)
}

// fitMetric works out how big to draw value and unit so the pair fits the
// cell: starting from maxSize (0 means "as tall as the cell allows"), shrink
// until it's no wider than the cell less its margins. It returns the value's
// and unit's font sizes, the gap between them, and their widths.
func fitMetric(c *render.Canvas, r image.Rectangle, value, unit string, maxSize float64) (size, unitSize float64, gap, wv, wu int) {
	size = maxSize
	if size <= 0 {
		size = float64(r.Dy()) * 0.62
	}
	for ; ; size *= 0.95 {
		unitSize, gap = size*0.30, int(size*0.08)
		wv = c.TextWidth(value, size, render.Bold)
		wu = c.TextWidth(unit, unitSize, render.Bold)
		if wv+gap+wu <= r.Dx()-80 || size < 40 {
			return
		}
	}
}

// Header draws the top bar: the page title, and a loud inverted banner
// whenever the server link is down or data has stopped arriving.
// Lost reports whether the black NO DATA banner is showing: the server link
// is down or nothing has arrived recently.
func Lost(s signalk.Snapshot, now time.Time) bool {
	return !s.Connected || !s.LastMessage.After(now.Add(-StaleAfter))
}

func Header(c *render.Canvas, title string, s signalk.Snapshot, now time.Time, e Env) {
	b := c.Bounds()
	lost := Lost(s, now)
	// The clock is drawn by us, in the title's font, since the stock Kindle
	// status bar is gone once we own the screen. It's in local time, so the
	// device's timezone has to be set correctly.
	clock := now.Format("15:04")
	if lost {
		c.FillRect(image.Rect(0, 0, b.Dx(), headerH), render.Black)
		c.Cog(cogX, cogY, cogR, render.White, render.Black) // settings stay reachable with no data
		// Where the title goes, not the middle: that is the battery's.
		c.Text(titleX, 64, "NO DATA", 60, render.Bold, render.Left, render.White)
		drawBattery(c, b.Dx()/2, int(cogY), e.Battery, render.White, render.Black)
		c.Text(b.Dx()-40, 64, clock, 56, render.Bold, render.Right, render.White)
		drawHeartbeat(c, b.Dx(), HeartbeatOn(now), render.Black)
	} else {
		c.Cog(cogX, cogY, cogR, render.Black, render.White)
		c.Text(titleX, 64, title, 56, render.Bold, render.Left, render.Black)
		drawBattery(c, b.Dx()/2, int(cogY), e.Battery, render.Black, render.White)
		c.Text(b.Dx()-40, 64, clock, 56, render.Bold, render.Right, render.Black)
		drawHeartbeat(c, b.Dx(), HeartbeatOn(now), render.White)
	}
	c.HLine(0, b.Dx(), headerH, 4, render.Black)
}

// The Nav page is a grid of eight boxes, two across and four down, numbered
// left to right then top to bottom. Each shows whatever the user picked
// (see BoxKinds); by default speed over ground, heading, depth, course over
// ground, velocity made good, the closest AIS contact, and the apparent wind
// speed and angle.
const navCols, navRows = 2, 4

// navCell is box i (0..5) of the Nav grid on a canvas with bounds b.
func navCell(b image.Rectangle, i int) image.Rectangle {
	top := headerH + 4
	w, h := b.Dx()/navCols, (b.Dy()-top)/navRows
	col, row := i%navCols, i/navCols
	return image.Rect(col*w, top+row*h, (col+1)*w, top+(row+1)*h)
}

// vmg is velocity made good to the wind: boat speed times the cosine of the
// true wind angle, so it's the speed we're actually closing on the wind (or,
// running downwind, on the point it blows towards). It uses the speed through
// the water when we have it - the usual definition - else the speed over the
// ground, and always needs the true wind direction and our heading.
func vmg(own signalk.Own, now time.Time) (float64, bool) {
	if !own.TWD.Fresh(now, StaleAfter) || !own.Heading.Fresh(now, StaleAfter) {
		return 0, false
	}
	speed := own.STW
	if !speed.Fresh(now, StaleAfter) {
		speed = own.SOG
	}
	if !speed.Fresh(now, StaleAfter) {
		return 0, false
	}
	return speed.V * math.Abs(math.Cos(own.TWD.V-own.Heading.V)), true
}

// Nav is the eight-box dashboard, in the user's chosen units. What each box
// shows is Env.Boxes (the user's choice, from the settings screens).
func Nav(c *render.Canvas, s signalk.Snapshot, now time.Time, e Env) {
	Header(c, "NAV", s, now, e)
	b := c.Bounds()
	own := s.Own
	kinds := NormalizeBoxes(e.Boxes)

	// One size for every box, from the widest thing likely to appear, so the
	// numbers line up and don't change size as values come and go. A value
	// too wide for it still shrinks to fit its own box.
	size, _, _, _, _ := fitMetric(c, navCell(b, 0), "88.8", "km/h", 0)

	var contacts []ais.Contact
	haveFix := own.Pos.Fresh(now, StaleAfter)
	if haveFix {
		contacts = ais.Contacts(own, s.Targets, now, StaleAfter)
	}
	for i, id := range kinds {
		cell := navCell(b, i)
		if id == BoxAIS {
			drawClosestAISBox(c, cell, contacts, haveFix, e, size)
			continue
		}
		drawMetric(c, cell, boxMetric(id, own, now, e, contacts), size)
	}

	// Grid lines last. They're static, so mid-grey is fine.
	for row := 1; row < navRows; row++ {
		c.HLine(0, b.Dx(), navCell(b, row*navCols).Min.Y-2, 3, render.Mid)
	}
	c.VLine(b.Dx()/navCols-1, headerH+4, b.Dy(), 3, render.Mid)
}

// drawClosestAISBox fills a box with the closest AIS contact, like any other
// box: its distance as the big number, its name in the label ("AIS AURA"), and
// under the number whether it is CLOSING or OPENING (getting nearer or further).
// With no contact it says NONE - or shows "--" when there's no position fix to
// measure from. size is the value size every box shares.
func drawClosestAISBox(c *render.Canvas, r image.Rectangle, contacts []ais.Contact, haveFix bool, e Env, size float64) {
	if len(contacts) == 0 {
		drawMetric(c, r, metric{label: "CLOSEST AIS", value: "NONE", ok: haveFix}, size)
		return
	}
	k := contacts[0] // nearest first
	value, unit := e.Units.Format("range", k.Range)
	label := fitText(c, "AIS "+strings.ToUpper(k.DisplayName()), 52, render.Bold, r.Dx()-80)
	drawMetric(c, r, metric{label: label, value: value, unit: unit, ok: true}, size)

	word := "OPENING"
	if k.Closing {
		word = "CLOSING"
	}
	// Solid black, like the number: this changes while the screen is up.
	c.Text((r.Min.X+r.Max.X)/2, r.Max.Y-20, word, 46, render.Bold, render.Center, render.Black)
}
