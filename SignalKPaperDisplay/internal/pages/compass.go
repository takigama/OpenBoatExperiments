package pages

import (
	"fmt"
	"image"
	"math"
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
	Header(c, "COMPASS", s, now)
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
	shade := render.Black
	heading := degrees(own.Heading.V)
	if !ok {
		shade = render.Mid
		heading = 0 // a frozen card would look live, so park it at north
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
		text = fmt.Sprintf("%03.0f°", heading)
	}
	c.Text(int(cx), int(cy+r*0.20), text, r*0.5, render.Bold, render.Center, shade)

	// AIS contacts, at their bearing relative to our bow. Bearings are
	// meaningless without a live heading and position, so with either
	// missing no blips are drawn at all.
	if ok && own.Pos.Fresh(now, StaleAfter) {
		drawAIS(c, cx, cy, r, own.Heading.V, ais.Contacts(own, s.Targets, now, StaleAfter), e)
	}

	// Apparent wind: shown only while it's live. The card is heading-up, so
	// the pointer sits at the wind angle measured from the bow. Drawn after
	// the blips so it stays on top where they coincide.
	if own.AWA.Fresh(now, StaleAfter) {
		drawWindPointer(c, cx, cy, r, own.AWA.V)
	}

	// Course over ground, against the heading line: where the boat is really
	// going, which differs from where it's pointing when there's leeway or
	// current. Only meaningful while we're actually moving.
	if ok && own.COG.Fresh(now, StaleAfter) && own.SOG.Fresh(now, StaleAfter) && own.SOG.V >= cogMinSpeed {
		drawCOG(c, cx, cy, r, own.COG.V-own.Heading.V, shade)
	}

	// The bow line: straight up from just above the heading digits to the
	// very top of the compass area, just under the header. The card is
	// heading-up, so this is the boat's direction of travel, and the wind
	// pointer and AIS blips are read against it. It goes last so nothing
	// else can leave a gap in it.
	c.Line(cx, cy-r*0.21, cx, float64(top+2), 8, shade)

	boxBottom := top + compassH
	drawWaterTemp(c, own, now, e, b.Dx()-28, top+10)
	drawFuelGauges(c, own.Fuel, now, b.Dx()-28, boxBottom-8)

	// Split row underneath: speed | depth.
	c.HLine(0, b.Dx(), boxBottom, 3, render.Mid)
	c.VLine(b.Dx()/2-1, boxBottom, b.Dy(), 3, render.Mid)

	sogVal, sogUnit := e.Units.Format("sog", own.SOG.V)
	depthVal, depthUnit := e.Units.Format("depth", own.Depth.V)
	drawMetric(c, image.Rect(0, boxBottom+3, b.Dx()/2-1, b.Dy()),
		metric{label: "SPEED", unit: sogUnit, ok: own.SOG.Fresh(now, StaleAfter), value: sogVal})
	drawMetric(c, image.Rect(b.Dx()/2+2, boxBottom+3, b.Dx(), b.Dy()),
		metric{label: "DEPTH", unit: depthUnit, ok: own.Depth.Fresh(now, StaleAfter), value: depthVal})
}

// cogMinSpeed is the slowest we'll trust a course over ground at: GPS
// course is noise when barely moving. 0.3 m/s is about 0.6 knots.
const cogMinSpeed = 0.3

// drawCOG draws the course-over-ground line, rel radians clockwise from the
// bow: a thin stem spanning just the outer part of the card and a little
// past the compass ring, finished with a short crossbar like a stretched T.
// It's much shorter and thinner than the heading line, which runs from the
// middle to the top of the compass area.
func drawCOG(c *render.Canvas, cx, cy, r, rel float64, shade uint8) {
	const (
		stem     = 5
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
// on us, hollow if the gap is growing. The nearest few also get their range,
// in the user's distance unit.
func drawAIS(c *render.Canvas, cx, cy, r, heading float64, contacts []ais.Contact, e Env) {
	const maxMarkers, maxLabels = 12, 3
	if len(contacts) > maxMarkers {
		contacts = contacts[:maxMarkers]
	}
	pos := func(k ais.Contact, radius float64) (x, y, ux, uy float64) {
		a := k.Bearing - heading
		ux, uy = math.Sin(a), -math.Cos(a)
		return cx + ux*radius, cy + uy*radius, ux, uy
	}

	// Furthest first, so the nearest ends up on top where they overlap.
	for i := len(contacts) - 1; i >= 0; i-- {
		k := contacts[i]
		px, py, ux, uy := pos(k, r)
		h := aisMarkerSize(k.Range)
		c.FillPolygon(diamond(px, py, ux, uy, h*1.4+9, h+9), render.White) // halo over the ticks
		c.FillPolygon(diamond(px, py, ux, uy, h*1.4, h), render.Black)
		if !k.Closing {
			c.FillPolygon(diamond(px, py, ux, uy, h*0.7, h*0.5), render.White) // hollow
		}
	}

	// Labels, nearest first. Each tries successively deeper positions along
	// its own bearing and takes the first that doesn't land on a label that's
	// already been placed - ships that are close together would otherwise
	// print their ranges on top of one another.
	var placed []image.Rectangle
	for i := 0; i < len(contacts) && i < maxLabels; i++ {
		k := contacts[i]
		value, unit := e.Units.Format("range", k.Range)
		label := value + " " + unit
		w := c.TextWidth(label, 32, render.Bold)

		var cands []image.Point
		for _, depth := range []float64{96, 142, 188} {
			lx, ly, _, _ := pos(k, r-depth)
			// A contact nearly dead ahead would put its label on the bow
			// line; slide the label to whichever side it's on, clear of it.
			if gap := w/2 + 14; math.Abs(lx-cx) < float64(gap) {
				if lx < cx {
					lx = cx - float64(gap)
				} else {
					lx = cx + float64(gap)
				}
			}
			cands = append(cands, image.Pt(int(lx), int(ly)))
		}
		at, box := placeLabel(cands, w, placed)
		placed = append(placed, box)
		c.TextHalo(at.X, at.Y+11, label, 32, render.Bold, render.Center, render.Black, render.White, 4)
	}
}

const labelH = 44

// labelBox is the area a label of width w occupies when centred on p.
func labelBox(p image.Point, w int) image.Rectangle {
	return image.Rect(p.X-w/2-8, p.Y-labelH/2, p.X+w/2+8, p.Y+labelH/2)
}

// placeLabel returns the first candidate position whose box doesn't overlap
// any already placed; if every one does, it settles for the last.
func placeLabel(cands []image.Point, w int, placed []image.Rectangle) (image.Point, image.Rectangle) {
	for _, p := range cands {
		box := labelBox(p, w)
		clear := true
		for _, q := range placed {
			if box.Overlaps(q) {
				clear = false
				break
			}
		}
		if clear {
			return p, box
		}
	}
	last := cands[len(cands)-1]
	return last, labelBox(last, w)
}

// drawWindPointer draws a bold arrowhead on the compass rim, pointing in
// toward the centre, at angle (radians clockwise from the bow). A white halo
// goes down first so it stays readable where it crosses the ticks.
func drawWindPointer(c *render.Canvas, cx, cy, r, angle float64) {
	ux, uy := math.Sin(angle), -math.Cos(angle) // outward from the centre
	vx, vy := -uy, ux                           // across it
	tri := func(tipR, baseR, half float64) []image.Point {
		pt := func(along, across float64) image.Point {
			return image.Pt(int(math.Round(cx+ux*along+vx*across)), int(math.Round(cy+uy*along+vy*across)))
		}
		return []image.Point{pt(tipR, 0), pt(baseR, -half), pt(baseR, half)}
	}
	c.FillPolygon(tri(r-92, r+40, 42), render.White) // halo
	c.FillPolygon(tri(r-76, r+34, 30), render.Black)
}

// rightValue draws "value unit" with the pair's right edge at xRight.
func rightValue(c *render.Canvas, xRight, baseline int, value, unit string, size float64, shade uint8) {
	unitSize := size * 0.45
	wu := c.TextWidth(unit, unitSize, render.Bold)
	c.Text(xRight, baseline, unit, unitSize, render.Bold, render.Right, render.Dark)
	c.Text(xRight-wu-int(size*0.08), baseline, value, size, render.Bold, render.Right, shade)
}

// drawWaterTemp draws the temperature in the top-right corner of the
// compass area - but only once the server has sent one.
func drawWaterTemp(c *render.Canvas, own signalk.Own, now time.Time, e Env, xRight, yTop int) {
	if !own.WaterTemp.Valid() {
		return
	}
	value, unit := "--", ""
	shade := render.Mid
	_, unit = e.Units.Format("watertemp", 0)
	if own.WaterTemp.Fresh(now, SlowStaleAfter) {
		value, _ = e.Units.Format("watertemp", own.WaterTemp.V)
		shade = render.Black
	}
	c.Text(xRight, yTop+34, "WATER", 34, render.Bold, render.Right, render.Dark)
	rightValue(c, xRight, yTop+34+66, value, unit, 68, shade)
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
	c.Text(xRight, barBottom-barH-14, "FUEL", 34, render.Bold, render.Right, render.Dark)

	for i, t := range tanks {
		x1 := xRight - (len(tanks)-1-i)*pitch
		x0 := x1 - barW
		fresh := t.Level.Fresh(now, SlowStaleAfter)

		frame := render.Black
		if !fresh {
			frame = render.Mid
		}
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
