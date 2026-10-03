package pages

import (
	"image"

	"signalkpaperdisplay/internal/render"
)

// The power screens: the dashboard owns the Kindle's screen and its power
// button, so the way to switch the Kindle off, restart it or get its own
// software back has to be here. Three choices, each asked about again before
// anything happens. The physical power button opens this screen too.

// The three things the Kindle can be asked to do, as the app and the screens
// name them.
const (
	PowerStock   = "stock"    // hand the Kindle back to its own software
	PowerRestart = "restart"  // reboot; the dashboard starts again by itself
	PowerOff     = "poweroff" // switch off; the power button starts it again
)

// PowerChoice is one of the three: what the list shows for it, what the
// confirmation asks, and what the screen says once it is done (an e-ink screen
// keeps its last picture with no power, so that picture is the message left
// behind).
type PowerChoice struct {
	ID       string
	Label    string
	Sub      string
	Question string
	Detail   string
	Farewell string
	Note     string // under the farewell
}

// PowerChoices are the choices, top to bottom.
var PowerChoices = []PowerChoice{
	{PowerStock, "KINDLE SOFTWARE", "Stop the dashboard, back to the Kindle's own screens",
		"Back to the Kindle's own software?", "The dashboard stays off until you delete\n/mnt/us/signalk/disable (over ssh).",
		"KINDLE SOFTWARE", "The dashboard is off. To bring it back, delete\n/mnt/us/signalk/disable"},
	{PowerRestart, "RESTART", "Reboot; the dashboard starts again by itself",
		"Restart the Kindle?", "It takes a minute or two to come back.",
		"RESTARTING", "The dashboard comes back in a minute or two"},
	{PowerOff, "POWER OFF", "Switch the Kindle off",
		"Power the Kindle off?", "Press its power button to start it again.",
		"POWERED OFF", "Press the power button to start"},
}

// PowerChoiceByID finds a choice.
func PowerChoiceByID(id string) (PowerChoice, bool) {
	for _, c := range PowerChoices {
		if c.ID == id {
			return c, true
		}
	}
	return PowerChoice{}, false
}

const (
	powerTop    = 190
	powerBtnH   = 190
	powerBtnGap = 40
)

// The confirmation's two buttons.
var (
	confirmYes    = image.Rect(60, 620, 520, 800)
	confirmCancel = image.Rect(552, 620, 1012, 800)
)

// powerButton is the rectangle of choice i on the power screen.
func powerButton(i, width int) image.Rectangle {
	y := powerTop + i*(powerBtnH+powerBtnGap)
	return image.Rect(60, y, width-60, y+powerBtnH)
}

// powerTap is what a tap does on the power screen or its confirmation.
func powerTap(v SettingsView, pt image.Point, width int) Action {
	if v.Screen == SettingsPowerConfirm {
		switch {
		case pt.In(confirmYes):
			return Action{Kind: ActPowerDo, Value: v.Power}
		case pt.In(confirmCancel):
			return Action{Kind: ActBack}
		}
		return Action{}
	}
	for i, c := range PowerChoices {
		if pt.In(powerButton(i, width)) {
			return Action{Kind: ActPowerPick, Value: c.ID}
		}
	}
	return Action{}
}

// drawPower draws the power screen and its confirmation. Black and white only.
func drawPower(c *render.Canvas, v SettingsView) {
	b := c.Bounds()
	if v.Screen == SettingsPowerConfirm {
		ch, ok := PowerChoiceByID(v.Power)
		if !ok {
			return
		}
		c.Text(b.Dx()/2, 330, ch.Question, 56, render.Bold, render.Center, render.Black)
		lines := splitLines(ch.Detail)
		for i, l := range lines {
			c.Text(b.Dx()/2, 420+i*54, l, 40, render.Regular, render.Center, render.Black)
		}
		c.FillRect(confirmYes, render.Black)
		c.Text((confirmYes.Min.X+confirmYes.Max.X)/2, (confirmYes.Min.Y+confirmYes.Max.Y)/2+22, "YES", 70, render.Bold, render.Center, render.White)
		c.FillRect(confirmCancel, render.Black)
		c.FillRect(confirmCancel.Inset(5), render.White)
		c.Text((confirmCancel.Min.X+confirmCancel.Max.X)/2, (confirmCancel.Min.Y+confirmCancel.Max.Y)/2+22, "CANCEL", 70, render.Bold, render.Center, render.Black)
		return
	}
	for i, ch := range PowerChoices {
		r := powerButton(i, b.Dx())
		c.FillRect(r, render.Black)
		c.FillRect(r.Inset(5), render.White)
		c.Text(r.Min.X+40, r.Min.Y+84, ch.Label, 66, render.Bold, render.Left, render.Black)
		c.Text(r.Min.X+40, r.Min.Y+142, fitText(c, ch.Sub, 36, render.Regular, r.Dx()-80), 36, render.Regular, render.Left, render.Black)
	}
	last := powerButton(len(PowerChoices)-1, b.Dx())
	c.Text(b.Dx()/2, last.Max.Y+70, "The Kindle's power button opens this screen too.", 36, render.Regular, render.Center, render.Dark)
}

// Farewell draws the last picture before the Kindle is switched off,
// restarted or handed back: the screen keeps it, so it says what happened.
func Farewell(c *render.Canvas, id string) {
	ch, ok := PowerChoiceByID(id)
	if !ok {
		return
	}
	b := c.Bounds()
	c.Text(b.Dx()/2, b.Dy()/2-20, ch.Farewell, 100, render.Bold, render.Center, render.Black)
	for i, l := range splitLines(ch.Note) {
		c.Text(b.Dx()/2, b.Dy()/2+60+i*52, l, 40, render.Regular, render.Center, render.Black)
	}
}

func splitLines(s string) []string {
	var out []string
	start := 0
	for i := 0; i <= len(s); i++ {
		if i == len(s) || s[i] == '\n' {
			out = append(out, s[start:i])
			start = i + 1
		}
	}
	return out
}
