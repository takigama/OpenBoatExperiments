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
)

// SlowStaleAfter is the staleness limit for values that change slowly (fuel
// level, water temperature): sensors for those often report every few
// seconds or even minutes, so the 5s limit used for speed and heading would
// flash them to "--" constantly.
const SlowStaleAfter = 2 * time.Minute

// compassLabel names the points of the card: N/E/S/W at the cardinals, then
// the usual compass-card convention of degrees/10 (3, 6, 12, 15 ...).
func compassLabel(deg int) string {
	switch deg {
	case 0:
		return "N"
	case 90:
		return "E"
	case 180:
		return "S"
	case 270:
		return "W"
	}
	return fmt.Sprintf("%d", deg/10)
}

// Compass shows a large rotating compass card (heading up, with a fixed
// marker at the top) over a split row with speed on the left and depth on
// the right. Inside the compass area: a wind pointer on the rim, water
// temperature top right, and fuel gauges bottom right. Each of those only
// appears once the server has actually sent that value - nothing is drawn
// for data the boat doesn't have.
func Compass(c *render.Canvas, s signalk.Snapshot, now time.Time, e Env) {
	Header(c, "COMPASS", s, now, e)
	b := c.Bounds()
	own := s.Own

	top := headerH + 4
	// The compass gets most of the screen; speed and depth are just a
	// compact strip underneath. It's limited by whichever is tighter: the
	// screen width, or the height left once the lubber marker has its room.
	compassH := (b.Dy() - top) * 78 / 100
	cx, cy := float64(b.Dx())/2, float64(top)+float64(compassH)/2
	r := math.Min(float64(b.Dx())/2-30, float64(compassH)/2-62)

	ok := own.Heading.Fresh(now, StaleAfter)
	shade := render.Black // solid black always: see the note on shades in drawMetric
	heading := degrees(own.Heading.V)
	if !ok {
		heading = 0 // a frozen card would look live, so park it at north (and show "--")
	}

	c.Ring(cx, cy, r, 6, shade)

	// Ticks and labels rotate with the card so the heading sits at the top.
	// Labels stay upright - only their positions rotate.
	for deg := 0; deg < 360; deg += 5 {
		theta := (float64(deg) - heading) * math.Pi / 180
		sin, cos := math.Sin(theta), math.Cos(theta)
		length, thick := 20.0, 3
		switch {
		case deg%30 == 0:
			length, thick = 56, 7
		case deg%10 == 0:
			length, thick = 36, 4
		}
		c.Line(cx+r*sin, cy-r*cos, cx+(r-length)*sin, cy-(r-length)*cos, thick, shade)

		if deg%30 == 0 {
			size, w := 46.0, render.Bold
			if deg%90 == 0 {
				size = 76
			}
			lr := r - 56 - size*0.75
			c.Text(int(cx+lr*sin), int(cy-lr*cos+size*0.36), compassLabel(deg), size, w, render.Center, shade)
		}
	}

	// Heading in the middle of the card.
	text := "--"
	if ok {
		text = fmt.Sprintf("%03.0f", heading)
	}
	c.Text(int(cx), int(cy+r*headingDrop), text, r*0.5, render.Bold, render.Center, shade)

	var contacts []ais.Contact // nearest first; stays empty without a fix and heading
	// AIS contacts, at their bearing relative to our bow. Bearings are
	// meaningless without a live heading and position, so with either
	// missing no blips are drawn at all.
	if ok && own.Pos.Fresh(now, StaleAfter) {
		contacts = ais.Contacts(own, s.Targets, now, StaleAfter)
		drawAIS(c, cx, cy, r, own.Heading.V, contacts)
	}

	// The wind, as two markers: the apparent wind (an "A": a solid head over two
	// hollow legs) and the true wind (a solid arrowhead). The card is heading-up,
	// so each sits at its wind angle measured from the bow, pointing in. They are
	// drawn after the blips so they stay on top where they coincide, and the true
	// one is left out while it is within windMarkersMerge of the apparent one,
	// where two markers would only overlap.
	apparent, hasApparent := own.AWA.V, own.AWA.Fresh(now, StaleAfter)
	trueRel, hasTrue := trueWindAngle(own, now)
	if hasTrue && (!hasApparent || angleDiff(apparent, trueRel) > windMarkersMerge) {
		drawTrueWindPointer(c, cx, cy, r, trueRel)
	}
	if hasApparent {
		drawApparentWindPointer(c, cx, cy, r, apparent)
	}

	// The next waypoint: a hollow arrowhead on the rim pointing outward, the
	// mirror image of the wind pointer (which points in). It's at the bearing
	// to the waypoint measured from the bow, so steering to put it on the bow
	// line heads for the waypoint. Only while the server is sending one.
	if ok && own.WPBearing.Fresh(now, StaleAfter) {
		drawWaypointPointer(c, cx, cy, r, own.WPBearing.V-own.Heading.V)
	}

	// Course over ground: where the boat is really going, which differs from
	// where it's pointing when there's leeway or current. Only meaningful
	// while we're actually moving, so neither the readout nor the line is
	// drawn otherwise. Both are relative to the heading, so both need it live.
	if cogLive(own, now) && ok {
		drawCOGOffset(c, cx, cy, r, own.COG.V, own.Heading.V)
		drawCOG(c, cx, cy, r, own.COG.V-own.Heading.V, shade)
	}

	// The bow line: straight up from just above the heading digits to the
	// very top of the compass area, just under the header. The card is
	// heading-up, so this is the boat's direction of travel, and the wind
	// pointer and AIS blips are read against it. It goes last so nothing
	// else can leave a gap in it.
	c.Line(cx, cy-r*(headingDrop+0.29), cx, float64(top+2), 4, shade)

	// The four corners of the compass area, each only when there's something
	// to show: closest ship top-left, water temperature top-right, wind speed
	// bottom-left, fuel bottom-right.
	boxBottom := top + compassH
	drawClosestAIS(c, contacts, e, 28, top+10)
	drawWaterTemp(c, own, now, e, b.Dx()-28, top+10)
	drawWindSpeed(c, own, now, e, 28, boxBottom-16)
	drawFuelGauges(c, own.Fuel, now, b.Dx()-28, boxBottom-8)

	// Split row underneath: speed | depth.
	c.HLine(0, b.Dx(), boxBottom, 3, render.Mid)
	c.VLine(b.Dx()/2-1, boxBottom, b.Dy(), 3, render.Mid)

	sogVal, sogUnit := e.Units.Format("sog", own.SOG.V)
	depthVal, depthUnit := e.Units.Format("depth", own.Depth.V)
	drawMetric(c, image.Rect(0, boxBottom+3, b.Dx()/2-1, b.Dy()),
		metric{label: "SPEED", unit: sogUnit, ok: own.SOG.Fresh(now, StaleAfter), value: sogVal}, 0)
	drawMetric(c, image.Rect(b.Dx()/2+2, boxBottom+3, b.Dx(), b.Dy()),
		metric{label: "DEPTH", unit: depthUnit, ok: own.Depth.Fresh(now, StaleAfter), value: depthVal}, 0)
}

// headingDrop is where the heading digits' baseline sits below the centre of
// the card, as a fraction of the radius. The heading and the COG offset under
// it are a block about 0.66r tall; this puts that block nearly centred on the
// card, and the bow line stops just above it.
const headingDrop = 0.06

// cogMinSpeed is the slowest we'll trust a course over ground at: GPS
// course is noise when barely moving. 0.3 m/s is about 0.6 knots.
const cogMinSpeed = 0.3

// cogLive reports whether there's a course over ground worth showing: fresh,
// and while we're actually making way.
func cogLive(own signalk.Own, now time.Time) bool {
	return own.COG.Fresh(now, StaleAfter) && own.SOG.Fresh(now, StaleAfter) && own.SOG.V >= cogMinSpeed
}

// cogOffset is how far the course over ground is from the heading, as whole
// degrees (0..180, the short way round) and which way: -1 means the course
// is to port of the heading, +1 to starboard, 0 means it's on the heading.
func cogOffset(cog, heading float64) (deg, dir int) {
	d := math.Mod(cog-heading, 2*math.Pi)
	if d > math.Pi {
		d -= 2 * math.Pi
	} else if d < -math.Pi {
		d += 2 * math.Pi
	}
	deg = int(math.Round(math.Abs(d) * 180 / math.Pi))
	switch {
	case deg == 0:
		return 0, 0
	case d < 0:
		return deg, -1
	}
	return deg, 1
}

// chevron draws one ">" (reach > 0) or "<" (reach < 0) whose back edge is at
// x and whose point is at x+reach, centred vertically on mid.
func chevron(c *render.Canvas, x, mid, reach, h float64) {
	c.Line(x, mid-h, x+reach, mid, 9, render.Black)
	c.Line(x+reach, mid, x, mid+h, 9, render.Black)
}

// drawCOGOffset shows the course-over-ground offset under the heading: two
// digits at 65% of the heading's size, with a chevron on whichever side the
// course lies. COG 29 on heading 39 reads "<10"; COG 29 on heading 19 reads
// "10>". The digits stay centred so they don't jump when the sign flips.
// Past 99 degrees there's no room for the digits, so three chevrons in the
// course's direction take their place: ">>>" or "<<<".
func drawCOGOffset(c *render.Canvas, cx, cy, r, cog, heading float64) {
	deg, dir := cogOffset(cog, heading)
	size := r * 0.5 * 0.65
	baseline := cy + r*headingDrop + r*0.06 + size*0.72
	mid := baseline - size*0.36 // the vertical middle of where the digits sit
	arm, h := size*0.24, size*0.30
	reach := arm // the point of each chevron, in its direction of travel
	if dir < 0 {
		reach = -arm
	}

	if deg > 99 {
		pitch, h := size*0.42, size*0.36
		for i := -1; i <= 1; i++ {
			chevron(c, cx+float64(i)*pitch-reach/2, mid, reach, h)
		}
		return
	}

	text := fmt.Sprintf("%02d", deg)
	c.Text(int(cx), int(baseline), text, size, render.Bold, render.Center, render.Black)
	if dir == 0 {
		return
	}
	half := float64(c.TextWidth(text, size, render.Bold)) / 2
	gap := size * 0.18
	x := cx + half + gap
	if dir < 0 {
		x = cx - half - gap
	}
	chevron(c, x, mid, reach, h)
}

// drawCOG draws the course-over-ground line, rel radians clockwise from the
// bow: a stem spanning just the outer part of the card and a little
// past the compass ring, finished with a short crossbar like a stretched T.
// It is much shorter than the heading line, which runs from the
// middle to the top of the compass area.
func drawCOG(c *render.Canvas, cx, cy, r, rel float64, shade uint8) {
	const (
		stem     = 10
		capHalf  = 18   // half the crossbar's length
		pastRing = 34   // how far beyond the ring the stem ends
		inner    = 0.60 // where the stem starts, as a fraction of the ring radius
	)
	// 0.60 clears the heading digits whichever way the line points: the
	// farthest corner of the box around them is only ~0.58 of the radius out.
	ux, uy := math.Sin(rel), -math.Cos(rel)
	end := r + pastRing
	c.Line(cx+ux*inner*r, cy+uy*inner*r, cx+ux*end, cy+uy*end, stem, shade)

	vx, vy := -uy, ux // across the line
	c.Line(cx+ux*end+vx*capHalf, cy+uy*end+vy*capHalf, cx+ux*end-vx*capHalf, cy+uy*end-vy*capHalf, stem, shade)
}

// diamond is a four-point shape centred on (px, py), a long along the
// outward direction (ux, uy) and b across it.
func diamond(px, py, ux, uy, a, b float64) []image.Point {
	vx, vy := -uy, ux
	pt := func(along, across float64) image.Point {
		return image.Pt(int(math.Round(px+ux*along+vx*across)), int(math.Round(py+uy*along+vy*across)))
	}
	return []image.Point{pt(a, 0), pt(0, b), pt(-a, 0), pt(0, -b)}
}

// aisMarkerSize makes nearer ships bigger, so range reads at a glance.
func aisMarkerSize(rangeM float64) float64 {
	switch {
	case rangeM <= 926: // half a mile
		return 34
	case rangeM <= 3704: // two miles
		return 27
	case rangeM <= 11112: // six miles
		return 21
	}
	return 17
}

// drawAIS puts a diamond on the rim for each contact: solid if it's closing
// on us, hollow if the gap is growing, bigger the nearer it is. The nearest
// contact's name and distance are written out in the top-left corner (see
// drawClosestAIS), not beside the diamonds, which would crowd the rim.
func drawAIS(c *render.Canvas, cx, cy, r, heading float64, contacts []ais.Contact) {
	const maxMarkers = 12
	if len(contacts) > maxMarkers {
		contacts = contacts[:maxMarkers]
	}

	// Furthest first, so the nearest ends up on top where they overlap.
	for i := len(contacts) - 1; i >= 0; i-- {
		k := contacts[i]
		a := k.Bearing - heading
		ux, uy := math.Sin(a), -math.Cos(a)
		px, py := cx+ux*r, cy+uy*r
		h := aisMarkerSize(k.Range)
		c.FillPolygon(diamond(px, py, ux, uy, h*1.4+9, h+9), render.White) // halo over the ticks
		c.FillPolygon(diamond(px, py, ux, uy, h*1.4, h), render.Black)
		if !k.Closing {
			c.FillPolygon(diamond(px, py, ux, uy, h*0.7, h*0.5), render.White) // hollow
		}
	}
}

// fitText shortens s with "..." until it's no wider than maxW.
func fitText(c *render.Canvas, s string, size float64, w render.Weight, maxW int) string {
	if c.TextWidth(s, size, w) <= maxW {
		return s
	}
	r := []rune(s)
	for len(r) > 1 {
		r = r[:len(r)-1]
		if t := string(r) + "..."; c.TextWidth(t, size, w) <= maxW {
			return t
		}
	}
	return "..."
}

// leftValue draws "value unit" with the pair's left edge at xLeft.
func leftValue(c *render.Canvas, xLeft, baseline int, value, unit string, size float64) {
	c.Text(xLeft, baseline, value, size, render.Bold, render.Left, render.Black)
	wv := c.TextWidth(value, size, render.Bold)
	c.Text(xLeft+wv+int(size*0.08), baseline, unit, size*0.45, render.Bold, render.Left, render.Black)
}

// drawClosestAIS shows the nearest AIS contact's name and distance in the
// top-left corner of the compass area, laid out like the water temperature
// opposite it. It is drawn only when there is a contact, and long names are
// shortened to fit the corner.
func drawClosestAIS(c *render.Canvas, contacts []ais.Contact, e Env, xLeft, yTop int) {
	if len(contacts) == 0 {
		return
	}
	k := contacts[0] // nearest first
	value, unit := e.Units.Format("range", k.Range)
	name := fitText(c, strings.ToUpper(k.DisplayName()), 34, render.Bold, 400)
	c.Text(xLeft, yTop+34, name, 34, render.Bold, render.Left, render.Black)
	leftValue(c, xLeft, yTop+34+66, value, unit, 68)
}

// drawWindSpeed shows the apparent wind speed in the bottom-left corner of
// the compass area, opposite the fuel gauges - but only once the server has
// sent one. Once seen, it shows "--" if it goes stale rather than vanishing.
func drawWindSpeed(c *render.Canvas, own signalk.Own, now time.Time, e Env, xLeft, yBottom int) {
	if !own.AWS.Valid() {
		return
	}
	value := "--"
	_, unit := e.Units.Format("aws", 0)
	if own.AWS.Fresh(now, StaleAfter) {
		value, _ = e.Units.Format("aws", own.AWS.V)
	}
	c.Text(xLeft, yBottom-112, "WIND", 57, render.Bold, render.Left, render.Black)
	leftValue(c, xLeft, yBottom, value, unit, 114)
}

// windMarkersMerge is how close, in radians (10 degrees), the true and apparent
// wind may be before only the apparent marker is drawn.
const windMarkersMerge = 10 * math.Pi / 180

// trueWindAngle is where the true wind comes from relative to the bow, radians
// clockwise from it, or false if it can't be worked out: it needs the true
// wind direction and our heading, both live.
func trueWindAngle(own signalk.Own, now time.Time) (float64, bool) {
	if !own.TWD.Fresh(now, StaleAfter) || !own.Heading.Fresh(now, StaleAfter) {
		return 0, false
	}
	return own.TWD.V - own.Heading.V, true
}

// angleDiff is the smallest angle between two bearings, in radians, 0..pi,
// whichever way round the circle is shorter.
func angleDiff(a, b float64) float64 {
	d := math.Mod(a-b, 2*math.Pi)
	if d < 0 {
		d += 2 * math.Pi
	}
	if d > math.Pi {
		d = 2*math.Pi - d
	}
	return d
}

// windFrame returns a function turning a point given as (along, across) the
// pointer - along its axis from the card's centre outwards, across it - into
// canvas coordinates, for a pointer at angle (radians clockwise from the bow).
func windFrame(cx, cy, angle float64) func(along, across float64) image.Point {
	ux, uy := math.Sin(angle), -math.Cos(angle) // outward from the centre
	vx, vy := -uy, ux                           // across it
	return func(along, across float64) image.Point {
		return image.Pt(int(math.Round(cx+ux*along+vx*across)), int(math.Round(cy+uy*along+vy*across)))
	}
}

// drawTrueWindPointer draws the true wind: a bold solid arrowhead on the
// compass rim, pointing in toward the centre, at angle (radians clockwise from
// the bow). A white halo goes down first so it stays readable where it crosses
// the ticks.
func drawTrueWindPointer(c *render.Canvas, cx, cy, r, angle float64) {
	pt := windFrame(cx, cy, angle)
	tri := func(tipR, baseR, half float64) []image.Point {
		return []image.Point{pt(tipR, 0), pt(baseR, -half), pt(baseR, half)}
	}
	c.FillPolygon(tri(r-92, r+40, 42), render.White) // halo
	c.FillPolygon(tri(r-76, r+34, 30), render.Black)
}

// The apparent wind marker is an "A": a solid triangular head, pointing in
// like the true wind's, over two hollow legs that carry its sides on down to
// the rim, with a narrow gap between them. About the size of the true wind
// arrowhead.
const (
	apparentLen    = 122.0 // tip to the foot of the legs
	apparentHalf   = 40.0  // half the width at the feet
	apparentHead   = 0.52  // the solid head's share of the length
	apparentGap    = 3.0   // half the gap between the legs at the top
	apparentBorder = 3.0   // line weight of the hollow legs
)

// drawApparentWindPointer draws the apparent wind marker at angle (radians
// clockwise from the bow), tip toward the centre.
func drawApparentWindPointer(c *render.Canvas, cx, cy, r, angle float64) {
	pt := windFrame(cx, cy, angle)
	tip := r - 76                                                          // the tip, as distance from the card's centre
	at := func(s, across float64) image.Point { return pt(tip+s, across) } // s: distance from the tip
	half := func(s float64) float64 { return apparentHalf * s / apparentLen }

	// Halo over the ticks, the whole shape's outline and a little more.
	c.FillPolygon([]image.Point{at(-12, 0), at(apparentLen+10, -apparentHalf-10), at(apparentLen+10, apparentHalf+10)}, render.White)

	// The solid head.
	headLen := apparentLen * apparentHead
	hw := half(headLen)
	c.FillPolygon([]image.Point{at(0, 0), at(headLen, -hw), at(headLen, hw)}, render.Black)

	// The legs: parallelograms whose outer edge continues the head's side and
	// whose inner edge runs parallel to it from beside the centre line. Each is
	// drawn solid then hollowed out.
	legW := hw - apparentGap // a leg's width across, at its top
	for _, side := range []float64{-1, 1} {
		quad := [][2]float64{ // (s, across)
			{headLen, side * hw}, {headLen, side * apparentGap},
			{apparentLen, side * (apparentHalf - legW)}, {apparentLen, side * apparentHalf},
		}
		pts := make([]image.Point, len(quad))
		var cs, ca float64
		for i, q := range quad {
			pts[i] = at(q[0], q[1])
			cs, ca = cs+q[0]/4, ca+q[1]/4
		}
		c.FillPolygon(pts, render.Black)
		// The inside: the same shape pulled in towards its middle by the line weight.
		inner := make([]image.Point, len(quad))
		ks := 1 - 2*apparentBorder/(apparentLen-headLen)
		ka := 1 - 2*apparentBorder/legW
		for i, q := range quad {
			inner[i] = at(cs+(q[0]-cs)*ks, ca+(q[1]-ca)*ka)
		}
		c.FillPolygon(inner, render.White)
	}
}

// drawWaypointPointer draws the waypoint marker at angle (radians clockwise
// from the bow): an arrowhead the same size as the wind pointer but turned
// the other way, so it points outward where the wind pointer points in, and
// hollow like an opening AIS diamond. A white halo goes down first so it stays
// readable over the ticks. A "W" is part of the marker, just inside its base
// and turned with it, so it can't be mistaken for the wind pointer.
func drawWaypointPointer(c *render.Canvas, cx, cy, r, angle float64) {
	ux, uy := math.Sin(angle), -math.Cos(angle) // outward from the centre
	vx, vy := -uy, ux                           // across it
	tri := func(tipR, baseR, half float64) []image.Point {
		pt := func(along, across float64) image.Point {
			return image.Pt(int(math.Round(cx+ux*along+vx*across)), int(math.Round(cy+uy*along+vy*across)))
		}
		return []image.Point{pt(tipR, 0), pt(baseR, -half), pt(baseR, half)}
	}
	// The wind pointer spans r-76 .. r+34 with its tip inward; this spans the
	// same band with the tip outward.
	const tip, base, half = 34.0, -76.0, 30.0
	c.FillPolygon(tri(r+tip+8, r+base-8, half+12), render.White) // halo
	c.FillPolygon(tri(r+tip, r+base, half), render.Black)
	// The hole: the same triangle scaled about its centre (a third of the way
	// from the base to the tip), leaving an outline of even-ish thickness.
	const k = 0.52
	centre := base + (tip-base)/3
	c.FillPolygon(tri(r+centre+(tip-centre)*k, r+centre+(base-centre)*k, half*k), render.White)

	// The W: part of the marker, so it turns with it. It sits on the pointer's
	// axis on the base side, its top (the side the triangle is on) waypointLabelGap
	// pixels clear of the base - at the top of the card the W is under the
	// triangle, at the bottom it is turned over, and so on round. The gap is
	// measured to the letter's ink, not the font's box.
	const size = 54.0
	inkW, inkH := c.InkSize("W", size, render.Bold)
	dist := r + base - waypointLabelGap - float64(inkH)/2 // from the card's centre to the ink's middle
	lx, ly := cx+ux*dist, cy+uy*dist
	// A white pad a pixel bigger all round, so it reads over the ticks.
	hw, hh := float64(inkW)/2+1, float64(inkH)/2+1
	corner := func(across, along float64) image.Point {
		return image.Pt(int(math.Round(lx+vx*across+ux*along)), int(math.Round(ly+vy*across+uy*along)))
	}
	c.FillPolygon([]image.Point{corner(-hw, -hh), corner(hw, -hh), corner(hw, hh), corner(-hw, hh)}, render.White)
	c.TextRotated(lx, ly, "W", size, render.Bold, angle, render.Black)
}

// waypointLabelGap is the clear space between the waypoint pointer's base and
// the W under it, in pixels.
const waypointLabelGap = 2.0

// rightValue draws "value unit" with the pair's right edge at xRight.
func rightValue(c *render.Canvas, xRight, baseline int, value, unit string, size float64, shade uint8) {
	unitSize := size * 0.45
	wu := c.TextWidth(unit, unitSize, render.Bold)
	c.Text(xRight, baseline, unit, unitSize, render.Bold, render.Right, render.Black)
	c.Text(xRight-wu-int(size*0.08), baseline, value, size, render.Bold, render.Right, shade)
}

// drawWaterTemp draws the temperature in the top-right corner of the
// compass area - but only once the server has sent one.
func drawWaterTemp(c *render.Canvas, own signalk.Own, now time.Time, e Env, xRight, yTop int) {
	if !own.WaterTemp.Valid() {
		return
	}
	// All solid black: this widget appears and updates mid-run, under the
	// fast black-and-white waveform, where grey would simply vanish.
	value := "--"
	_, unit := e.Units.Format("watertemp", 0)
	if own.WaterTemp.Fresh(now, SlowStaleAfter) {
		value, _ = e.Units.Format("watertemp", own.WaterTemp.V)
	}
	c.Text(xRight, yTop+34, "WATER", 34, render.Bold, render.Right, render.Black)
	rightValue(c, xRight, yTop+34+66, value, unit, 68, render.Black)
}

// drawFuelGauges draws one small bar per fuel tank the server reports,
// bottom-right of the compass area, with the right edge at xRight and the
// bottom at yBottom. With no tanks reported it draws nothing at all.
func drawFuelGauges(c *render.Canvas, tanks []signalk.Tank, now time.Time, xRight, yBottom int) {
	const (
		barW, barH = 46, 104
		pitch      = 78
		maxTanks   = 3
	)
	if len(tanks) == 0 {
		return
	}
	if len(tanks) > maxTanks {
		tanks = tanks[:maxTanks]
	}
	barBottom := yBottom - 38 // room under the bars for the percentage
	c.Text(xRight, barBottom-barH-14, "FUEL", 34, render.Bold, render.Right, render.Black)

	for i, t := range tanks {
		x1 := xRight - (len(tanks)-1-i)*pitch
		x0 := x1 - barW
		fresh := t.Level.Fresh(now, SlowStaleAfter)

		// Solid black either way; a stale tank is told apart by being empty
		// and labelled "--", since a grey frame would vanish under DU.
		frame := render.Black
		c.FillRect(image.Rect(x0, barBottom-barH, x1, barBottom), frame)
		c.FillRect(image.Rect(x0+4, barBottom-barH+4, x1-4, barBottom-4), render.White)

		label := "--"
		if fresh {
			level := math.Min(math.Max(t.Level.V, 0), 1)
			fill := int(level * float64(barH-8))
			c.FillRect(image.Rect(x0+4, barBottom-4-fill, x1-4, barBottom-4), render.Black)
			label = fmt.Sprintf("%.0f%%", level*100)
		}
		c.Text((x0+x1)/2, yBottom-6, label, 30, render.Bold, render.Center, frame)
	}
}
