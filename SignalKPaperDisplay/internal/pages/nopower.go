package pages

import (
	"fmt"

	"signalkpaperdisplay/internal/render"
)

// The no-power screen. After a while off external power the dashboard stops
// drawing and shows this: big, on two lines, so it can be read from across a
// cabin. An e-ink screen keeps its picture with no power, so this costs nothing
// to leave up while everything else sleeps.

// NoPowerChoice is one timeout the settings offer, in minutes (0 is never).
type NoPowerChoice struct {
	Minutes int
	Label   string
}

// NoPowerChoices are the timeouts on the settings screen, shortest first and
// never last. The web API accepts any whole number of minutes up to a week.
var NoPowerChoices = []NoPowerChoice{
	{1, "1 minute"}, {5, "5 minutes"}, {15, "15 minutes"}, {30, "30 minutes"},
	{60, "1 hour"}, {120, "2 hours"}, {240, "4 hours"}, {480, "8 hours"},
	{0, "Never"},
}

// NoPowerLabel names a timeout in minutes, whether or not it is one of the
// choices: "1 hour", "90 minutes", "Never".
func NoPowerLabel(minutes int) string {
	for _, c := range NoPowerChoices {
		if c.Minutes == minutes {
			return c.Label
		}
	}
	switch {
	case minutes <= 0:
		return "Never"
	case minutes%60 == 0 && minutes/60 == 1:
		return "1 hour"
	case minutes%60 == 0:
		return fmt.Sprintf("%d hours", minutes/60)
	case minutes == 1:
		return "1 minute"
	}
	return fmt.Sprintf("%d minutes", minutes)
}

// NoPowerScreen draws NO / POWER, as large as the screen's width allows, and a
// line underneath saying what brings it back.
func NoPowerScreen(c *render.Canvas) {
	b := c.Bounds()
	// The longer line, POWER, fills most of the width; NO uses the same size.
	const probe = 200.0
	w := float64(c.TextWidth("POWER", probe, render.Bold))
	size := probe * float64(b.Dx()) * 0.86 / w
	mid := b.Dy() * 44 / 100
	c.Text(b.Dx()/2, mid-int(size*0.12), "NO", size, render.Bold, render.Center, render.Black)
	c.Text(b.Dx()/2, mid+int(size*0.92), "POWER", size, render.Bold, render.Center, render.Black)
	c.Text(b.Dx()/2, mid+int(size*0.92)+110, "Plug in, or tap the screen, to resume", 44, render.Regular, render.Center, render.Black)
}
