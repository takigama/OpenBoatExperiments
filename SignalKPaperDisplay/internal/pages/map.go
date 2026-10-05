package pages

import (
	"fmt"
	"image"
	"math"
	"strings"
	"time"

	"signalkpaperdisplay/internal/ais"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// The map page: our boat in the middle of a round map out to a chosen range,
// with the AIS ships around it, where they are and which way they are heading -
// a small radar picture. Heading-up by default (ahead is up the screen, like the
// compass card), or north-up. The wind is drawn on the outside of the ring.
//
// Everything is solid black: a ship moves, and the screen is redrawn in place
// under a waveform that has no grey, so nothing here may depend on one.

// MapRanges are the ranges, in nautical miles, the map can show; 5 is the default.
var MapRanges = []int{1, 2, 5, 10}

// DefaultMapRange is the range, in nautical miles, until it is changed.
const DefaultMapRange = 5

const nauticalMile = 1852.0

// MapRangeOK reports whether nm is one of the ranges the map has.
func MapRangeOK(nm int) bool {
	for _, r := range MapRanges {
		if r == nm {
			return true
		}
	}
	return false
}

// MapRangeOrDefault is the range an Env's setting stands for: zero is the default.
func MapRangeOrDefault(nm int) int {
	if nm == 0 {
		return DefaultMapRange
	}
	return nm
}

// NextMapRange is the range a tap moves on to, going round in a circle.
func NextMapRange(nm int) int {
	nm = MapRangeOrDefault(nm)
	for i, r := range MapRanges {
		if r == nm {
			return MapRanges[(i+1)%len(MapRanges)]
		}
	}
	return DefaultMapRange
}

// mapStripH is the strip under the map: the wind on the left, the ship that
// will pass closest on the right.
const mapStripH = 236

// mapLayout is where the map sits: its centre, its radius, and the top of the
// strip. The wind markers sit outside the ring, so the ring is smaller than the
// width allows.
func mapLayout(b image.Rectangle) (cx, cy, r float64, stripTop int) {
	top := headerH + 4
	stripTop = b.Dy() - mapStripH
	availH := float64(stripTop - top)
	cx = float64(b.Dx()) / 2
	cy = float64(top) + availH/2
	r = math.Min(float64(b.Dx())/2-126, availH/2-96)
	return cx, cy, r, stripTop
}

// MapOrientRect is the corner of the map that switches between heading-up and
// north-up when tapped; MapRangeRect the one that changes the range; and
// MapWindRect the wind widget in the strip, which switches true and apparent
// wind as on the compass. All a good deal bigger than their text.
func MapOrientRect(b image.Rectangle) image.Rectangle {
	return image.Rect(0, mapCornerTop, 330, mapCornerTop+180)
}

func MapRangeRect(b image.Rectangle) image.Rectangle {
	return image.Rect(b.Dx()-330, mapCornerTop, b.Dx(), mapCornerTop+180)
}

// mapCornerTop is where the corner tap areas start: just below the settings cog's,
// so a tap on the cog is never taken for one of these.
const mapCornerTop = headerH + 20

func MapWindRect(b image.Rectangle) image.Rectangle {
	return image.Rect(0, b.Dy()-mapStripH, 360, b.Dy())
}

// mapOffset is where a contact is on the map, as an offset from its centre in
// design units. up is the direction that is up the screen, a bearing in radians
// (our heading, or 0 for north-up).
func mapOffset(k ais.Contact, up, rangeM, r float64) (dx, dy float64) {
	a := k.Bearing - up
	d := k.Range / rangeM * r
	return math.Sin(a) * d, -math.Cos(a) * d
}

// rangeText writes a distance in the user's distance unit, without trailing
// zeros: "5 nm", "9.26 km".
func rangeText(m float64, u units.Settings) string {
	v, unit := u.Format("range", m)
	if strings.Contains(v, ".") {
		v = strings.TrimRight(strings.TrimRight(v, "0"), ".")
	}
	return v + " " + unit
}

// shipShape is a ship's symbol: a long triangle pointing along its course.
func shipShape(px, py, ang, length, half float64) []image.Point {
	ux, uy := math.Sin(ang), -math.Cos(ang)
	vx, vy := -uy, ux
	pt := func(along, across float64) image.Point {
		return image.Pt(int(math.Round(px+ux*along+vx*across)), int(math.Round(py+uy*along+vy*across)))
	}
	return []image.Point{pt(length, 0), pt(-length*0.7, -half), pt(-length*0.4, 0), pt(-length*0.7, half)}
}

// dashedRing draws a circle as a ring of dashes.
func dashedRing(c *render.Canvas, cx, cy, r float64, dashes int, thick int) {
	for i := 0; i < dashes; i++ {
		a0 := 2 * math.Pi * float64(i) / float64(dashes)
		a1 := a0 + math.Pi/float64(dashes)*0.9
		c.Line(cx+r*math.Sin(a0), cy-r*math.Cos(a0), cx+r*math.Sin(a1), cy-r*math.Cos(a1), thick, render.Black)
	}
}

// haloText draws text with a white box behind it, so a ring or a ship does not
// run through it.
func haloText(c *render.Canvas, x, baseline int, s string, size float64, a render.Align) {
	w := c.TextWidth(s, size, render.Bold)
	left := x
	switch a {
	case render.Center:
		left = x - w/2
	case render.Right:
		left = x - w
	}
	c.FillRect(image.Rect(left-6, baseline-int(size*0.85), left+w+6, baseline+int(size*0.3)), render.White)
	c.Text(x, baseline, s, size, render.Bold, a, render.Black)
}

// Map is the map page.
func Map(c *render.Canvas, s signalk.Snapshot, now time.Time, e Env) {
	Header(c, "MAP", s, now, e)
	b := c.Bounds()
	own := s.Own
	cx, cy, r, stripTop := mapLayout(b)

	nm := MapRangeOrDefault(e.MapRange)
	rangeM := float64(nm) * nauticalMile

	headingOK := own.Heading.Fresh(now, StaleAfter)
	northUp := e.MapNorthUp || !headingOK // with no heading there is no "ahead" to point up
	up := 0.0
	if !northUp {
		up = own.Heading.V
	}

	// The rings: the edge, a dashed one half way, and the range written on them.
	c.Ring(cx, cy, r, 4, render.Black)
	dashedRing(c, cx, cy, r/2, 40, 3)
	haloText(c, int(cx+r/2), int(cy)+12, rangeText(rangeM/2, e.Units), 32, render.Center)
	haloText(c, int(cx+r), int(cy)+12, rangeText(rangeM, e.Units), 34, render.Center)

	// North, on the rim.
	na := -up
	nx, ny := math.Sin(na), -math.Cos(na)
	c.Line(cx+nx*r, cy+ny*r, cx+nx*(r+22), cy+ny*(r+22), 5, render.Black)
	c.Text(int(cx+nx*(r+52)), int(cy+ny*(r+52))+14, "N", 40, render.Bold, render.Center, render.Black)

	// Wind, outside the ring: the apparent wind as the "A" marker and the true
	// wind as the arrowhead, as on the compass (the true one only while it
	// differs from the apparent).
	headingForWind := 0.0
	if headingOK {
		headingForWind = own.Heading.V
	}
	apparent, hasApparent := own.AWA.V, own.AWA.Fresh(now, StaleAfter) && headingOK
	trueRel, hasTrue := trueWindAngle(own, now)
	if northUp { // measured from north rather than from the bow
		apparent += headingForWind
		trueRel += headingForWind
	}
	const windOffset = 88.0
	if hasTrue && (!hasApparent || angleDiff(apparent, trueRel) > windMarkersMerge) {
		drawTrueWindPointer(c, cx, cy, r+windOffset, trueRel)
	}
	if hasApparent {
		drawApparentWindPointer(c, cx, cy, r+windOffset, apparent)
	}

	// The ships, furthest first so the nearest are on top.
	var contacts []ais.Contact // nearest first
	if own.Pos.Fresh(now, StaleAfter) {
		for _, k := range ais.Contacts(own, s.Targets, now, StaleAfter) {
			if k.Range <= rangeM {
				contacts = append(contacts, k)
			}
		}
	}
	urgent, haveUrgent := ais.MostUrgent(contacts)
	for i := len(contacts) - 1; i >= 0; i-- {
		drawMapShip(c, contacts[i], up, rangeM, cx, cy, r, haveUrgent && contacts[i].ID == urgent.ID)
	}
	// Names for the nearest few.
	for i := 0; i < len(contacts) && i < 5; i++ {
		k := contacts[i]
		dx, dy := mapOffset(k, up, rangeM, r)
		px, py := cx+dx, cy+dy
		name := short(k.DisplayName(), 10)
		w := c.TextWidth(name, 30, render.Bold)
		x, al := int(px)+26, render.Left
		if float64(x+w) > float64(b.Dx())-8 {
			x, al = int(px)-26, render.Right
		}
		haloText(c, x, int(py)+10, name, 30, al)
	}

	// Our boat, on top: an arrowhead, with its heading line out to the rim.
	hang := 0.0 // the bow, as an angle from up
	if headingOK {
		hang = own.Heading.V - up
	}
	if headingOK {
		hx, hy := math.Sin(hang), -math.Cos(hang)
		c.Line(cx+hx*46, cy+hy*46, cx+hx*r, cy+hy*r, 3, render.Black)
	}
	c.FillPolygon(shipShape(cx, cy, hang, 40, 24), render.White) // halo
	c.FillPolygon(shipShape(cx, cy, hang, 32, 18), render.Black)

	// The corners: which way is up, and the range, both changed by a tap.
	orient := "HDG UP"
	if northUp {
		orient = "N UP"
	}
	c.Text(28, headerH+4+64, orient, 52, render.Bold, render.Left, render.Black)
	c.Text(b.Dx()-28, headerH+4+64, rangeText(rangeM, e.Units), 52, render.Bold, render.Right, render.Black)
	c.Text(b.Dx()-28, headerH+4+112, "RANGE", 34, render.Bold, render.Right, render.Black)
	if !own.Pos.Fresh(now, StaleAfter) {
		haloText(c, int(cx), int(cy+r*0.55), "NO POSITION", 44, render.Center)
	}

	// The strip: the wind, and the ship that will pass closest.
	c.HLine(0, b.Dx(), stripTop, 3, render.Mid)
	c.VLine(b.Dx()/2-1, stripTop, b.Dy(), 3, render.Mid)
	drawWindSpeed(c, own, now, e, 28, b.Dy()-16)
	xr := b.Dx()/2 + 30
	if haveUrgent {
		c.Text(xr, stripTop+56, "CPA "+short(urgent.DisplayName(), 12), 40, render.Bold, render.Left, render.Black)
		v, u := e.Units.Format("range", urgent.CPA)
		leftValue(c, xr, stripTop+160, v, u, 96)
		tv, tu := formatDuration(urgent.TCPA)
		c.Text(xr, stripTop+212, fmt.Sprintf("in %s %s", tv, tu), 40, render.Bold, render.Left, render.Black)
	} else {
		c.Text(xr, stripTop+56, "NO CLOSING SHIPS", 40, render.Bold, render.Left, render.Black)
		c.Text(xr, stripTop+160, fmt.Sprintf("%d", len(contacts)), 96, render.Bold, render.Left, render.Black)
		noun := "ships in range"
		if len(contacts) == 1 {
			noun = "ship in range"
		}
		c.Text(xr+int(c.TextWidth(fmt.Sprintf("%d", len(contacts)), 96, render.Bold))+18, stripTop+160, noun, 40, render.Bold, render.Left, render.Black)
	}
}

// mapShipSize is half the width of a ship's symbol: nearer ships are drawn bigger,
// but none so small that it cannot be made out on the panel.
func mapShipSize(rangeM float64) float64 {
	switch {
	case rangeM <= 926: // half a mile
		return 30
	case rangeM <= 3704: // two miles
		return 26
	}
	return 22
}

// drawMapShip draws one ship: a triangle pointing along its course, solid if it is
// getting nearer and hollow if not, with a line showing where it will be in ten
// minutes, and a ring round the one that will pass closest. A ship that has not
// sent its course is a diamond.
func drawMapShip(c *render.Canvas, k ais.Contact, up, rangeM, cx, cy, r float64, urgent bool) {
	dx, dy := mapOffset(k, up, rangeM, r)
	px, py := cx+dx, cy+dy
	size := mapShipSize(k.Range)

	if k.HasMotion {
		ang := k.COG - up
		// Where it will be in ten minutes, if it keeps on.
		ux, uy := math.Sin(ang), -math.Cos(ang)
		reach := k.SOG * 600 / rangeM * r
		if reach > size*1.6 {
			ex, ey := px+ux*reach, py+uy*reach
			if d := math.Hypot(ex-cx, ey-cy); d > r { // not past the edge of the map
				f := (r - math.Hypot(px-cx, py-cy)) / (d - math.Hypot(px-cx, py-cy))
				if f < 0 {
					f = 0
				}
				ex, ey = px+ux*reach*f, py+uy*reach*f
			}
			c.Line(px+ux*size, py+uy*size, ex, ey, 4, render.White) // halo
			c.Line(px+ux*size, py+uy*size, ex, ey, 2, render.Black)
		}
		c.FillPolygon(shipShape(px, py, ang, size*1.5+8, size*0.9+8), render.White) // halo
		c.FillPolygon(shipShape(px, py, ang, size*1.5, size*0.9), render.Black)
		if !k.Closing {
			c.FillPolygon(shipShape(px, py, ang, size*0.8, size*0.4), render.White) // hollow
		}
	} else {
		c.FillPolygon(diamond(px, py, 0, -1, size*1.2+8, size+8), render.White)
		c.FillPolygon(diamond(px, py, 0, -1, size*1.2, size), render.Black)
		if !k.Closing {
			c.FillPolygon(diamond(px, py, 0, -1, size*0.6, size*0.45), render.White)
		}
	}
	if urgent {
		c.Ring(px, py, size*2.2, 4, render.Black)
	}
}
