package pages

import (
	"image"
	"testing"
	"time"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// The battery sits in the middle of the header, about x=536, y=46.
var headerMiddle = image.Rect(420, 14, 660, 80)

func renderWithBattery(t *testing.T, s signalk.Snapshot, st *battery.Status) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	Compass(c, s, compassNow, Env{Units: units.Settings{Preset: units.PresetMetric}, Battery: st})
	return c
}

func TestBatteryShownInTheMiddleOfTheHeaderOnlyWhenKnown(t *testing.T) {
	without := renderWithBattery(t, base(), nil)
	if n := inked(without, headerMiddle); n != 0 {
		t.Errorf("with no battery reading the middle of the header has %d inked pixels, want none", n)
	}
	with := renderWithBattery(t, base(), &battery.Status{Percent: 87})
	if inked(with, headerMiddle) == 0 {
		t.Error("a battery reading should be drawn in the middle of the header")
	}
	// And only there: the rest of the page is untouched.
	rest := image.Rect(0, 94, 1072, 1448)
	if inked(with, rest) != inked(without, rest) {
		t.Error("the battery changed something below the header")
	}
}

func TestBatteryFillFollowsTheCharge(t *testing.T) {
	full := renderWithBattery(t, base(), &battery.Status{Percent: 100})
	half := renderWithBattery(t, base(), &battery.Status{Percent: 50})
	low := renderWithBattery(t, base(), &battery.Status{Percent: 5})
	body := image.Rect(440, 26, 560, 66)
	if !(inked(full, body) > inked(half, body) && inked(half, body) > inked(low, body)) {
		t.Errorf("the fill should grow with the charge: %d, %d, %d", inked(full, body), inked(half, body), inked(low, body))
	}
	// Even a nearly flat battery shows something in the body.
	empty := renderWithBattery(t, base(), &battery.Status{Percent: 0})
	if inked(low, body) <= inked(empty, body) {
		t.Error("5% should show a sliver more than 0%")
	}
}

func TestBatteryPluggedShowsABolt(t *testing.T) {
	unplugged := renderWithBattery(t, base(), &battery.Status{Percent: 60})
	plugged := renderWithBattery(t, base(), &battery.Status{Percent: 60, Plugged: true, Charging: true})
	if !differs(unplugged, plugged) {
		t.Fatal("being plugged in should look different")
	}
	if inked(plugged, headerMiddle) <= inked(unplugged, headerMiddle) {
		t.Errorf("the bolt should add ink: %d vs %d", inked(plugged, headerMiddle), inked(unplugged, headerMiddle))
	}
	// Full and still plugged in: bolt, no further change needed to the fill.
	fullPlugged := renderWithBattery(t, base(), &battery.Status{Percent: 100, Plugged: true})
	fullBatt := renderWithBattery(t, base(), &battery.Status{Percent: 100})
	if !differs(fullPlugged, fullBatt) {
		t.Error("a full battery on a charger should still show it is plugged in")
	}
}

func TestBatteryUnknownLevelStillDrawsWhenPlugged(t *testing.T) {
	c := renderWithBattery(t, base(), &battery.Status{Percent: -1, Plugged: true})
	if inked(c, headerMiddle) == 0 {
		t.Error("an unknown level should draw dashes, not vanish")
	}
}

func TestBatteryOnTheNoDataBanner(t *testing.T) {
	// No data: the black banner, battery in white, title moved left so the
	// middle is free for it.
	lost := base()
	lost.Connected = false
	c, _ := render.NewCanvas(1072, 1448)
	Compass(c, lost, compassNow, Env{Units: units.Settings{Preset: units.PresetMetric}, Battery: &battery.Status{Percent: 40}})
	light := 0
	for y := headerMiddle.Min.Y; y < headerMiddle.Max.Y; y++ {
		for x := headerMiddle.Min.X; x < headerMiddle.Max.X; x++ {
			if c.Img.GrayAt(x, y).Y > 200 {
				light++
			}
		}
	}
	if light == 0 {
		t.Error("the battery should be drawn in white on the black banner")
	}
	// The title no longer sits in the middle where the battery is.
	c2, _ := render.NewCanvas(1072, 1448)
	Compass(c2, lost, compassNow, Env{Units: units.Settings{Preset: units.PresetMetric}})
	if inked(c2, headerMiddle) > 0 && lightIn(c2, headerMiddle) > 0 {
		t.Error("with no battery the middle of the banner should be bare, the NO DATA text being at the left")
	}
	_ = time.Now
}

func lightIn(c *render.Canvas, r image.Rectangle) int {
	n := 0
	for y := r.Min.Y; y < r.Max.Y; y++ {
		for x := r.Min.X; x < r.Max.X; x++ {
			if c.Img.GrayAt(x, y).Y > 200 {
				n++
			}
		}
	}
	return n
}
