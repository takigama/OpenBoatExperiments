package pages

import (
	"image"
	"time"

	"signalkpaperdisplay/internal/render"
)

// The heartbeat is a dot beside the clock that blinks once a second, so a
// glance tells you the app and the panel are alive: if it stops, something
// has hung. It's updated as a tiny region of its own rather than as part of
// the whole picture - otherwise every blink would be a whole-screen refresh.
const (
	heartbeatR    = 20.0 // about the height of the clock's digits
	heartbeatY    = 44.0
	heartbeatFrom = 40 + 150 + 38 // distance of its centre from the right edge: the clock's margin and widest width, then a gap
)

func heartbeatCenter(width int) (x, y float64) {
	return float64(width - heartbeatFrom), heartbeatY
}

// HeartbeatRect is the screen area the dot lives in, with a little margin.
func HeartbeatRect(width int) image.Rectangle {
	x, y := heartbeatCenter(width)
	const pad = 6
	return image.Rect(int(x-heartbeatR)-pad, int(y-heartbeatR)-pad, int(x+heartbeatR)+pad+1, int(y+heartbeatR)+pad+1)
}

// HeartbeatOn says whether the dot is lit at the given moment: on for one
// second, off for the next.
func HeartbeatOn(now time.Time) bool { return now.Unix()%2 == 0 }

// drawHeartbeat paints the dot (or clears its area) onto a canvas where the
// background is bg - white normally, black behind the NO DATA banner.
func drawHeartbeat(c *render.Canvas, width int, on bool, bg uint8) {
	if !on {
		return
	}
	x, y := heartbeatCenter(width)
	shade := render.Black
	if bg == render.Black {
		shade = render.White
	}
	c.Disc(x, y, heartbeatR, shade)
}

// HeartbeatImage renders just the dot's area, for updating it on its own.
// banner is true while the black NO DATA bar is showing, since the dot then
// has to be white on black to be seen at all.
func HeartbeatImage(width int, on, banner bool) (*image.Gray, image.Rectangle) {
	rect := HeartbeatRect(width)
	bg := render.White
	if banner {
		bg = render.Black
	}
	c, err := render.NewCanvas(width, rect.Max.Y+1)
	if err != nil {
		return nil, rect
	}
	c.FillRect(c.Bounds(), bg)
	drawHeartbeat(c, width, on, bg)
	return c.Img.SubImage(rect).(*image.Gray), rect
}
