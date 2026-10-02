package pages

import (
	"fmt"
	"image"
	"math"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
)

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
// the right.
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

	// Fixed lubber marker at the top, pointing in at the card.
	tipY := int(cy-r) - 6
	c.FillPolygon([]image.Point{
		{int(cx) - 30, tipY - 44}, {int(cx) + 30, tipY - 44}, {int(cx), tipY},
	}, render.Black)

	// Heading in the middle of the card.
	text := "--"
	if ok {
		text = fmt.Sprintf("%03.0f°", heading)
	}
	c.Text(int(cx), int(cy+r*0.20), text, r*0.5, render.Bold, render.Center, shade)

	// Split row underneath: speed | depth.
	splitTop := top + compassH
	c.HLine(0, b.Dx(), splitTop, 3, render.Mid)
	c.VLine(b.Dx()/2-1, splitTop, b.Dy(), 3, render.Mid)

	sogVal, sogUnit := e.Units.Format("sog", own.SOG.V)
	depthVal, depthUnit := e.Units.Format("depth", own.Depth.V)
	drawMetric(c, image.Rect(0, splitTop+3, b.Dx()/2-1, b.Dy()),
		metric{label: "SPEED", unit: sogUnit, ok: own.SOG.Fresh(now, StaleAfter), value: sogVal})
	drawMetric(c, image.Rect(b.Dx()/2+2, splitTop+3, b.Dx(), b.Dy()),
		metric{label: "DEPTH", unit: depthUnit, ok: own.Depth.Fresh(now, StaleAfter), value: depthVal})
}
