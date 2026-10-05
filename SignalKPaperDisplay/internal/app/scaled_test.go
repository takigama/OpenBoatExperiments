package app

import (
	"math"
	"path/filepath"
	"testing"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// smallApp is an app on a 600x800 screen - the Kindle Basic's - drawing the
// layout made for 1072 across, scaled.
func smallApp(t *testing.T) *App {
	t.Helper()
	a := &App{State: signalk.NewState(), Display: &display.PNG{W: 600, H: 800},
		Units: units.Settings{Preset: units.PresetMetric}, SettingsPath: filepath.Join(t.TempDir(), "s.json")}
	return a
}

// dev converts a point in design units to device pixels, as the touch panel
// would report a finger there.
func dev(x, y int) (int, int) {
	s := 600.0 / 1072
	return int(math.Round(float64(x) * s)), int(math.Round(float64(y) * s))
}

func TestFrameIsTheDevicesSize(t *testing.T) {
	a := smallApp(t)
	img, err := a.Frame(time.Unix(2000, 0))
	if err != nil {
		t.Fatal(err)
	}
	if b := img.Bounds(); b.Dx() != 600 || b.Dy() != 800 {
		t.Fatalf("frame is %v, want 600x800", b)
	}
	if w, h := a.designSize(); w != 1072 || h < 1427 || h > 1431 {
		t.Errorf("design size %dx%d, want 1072x1429", w, h)
	}
}

func TestTapsOnASmallScreenHitTheSameThingsAsOnTheBigOne(t *testing.T) {
	a := smallApp(t)
	a.SetPage("nav")
	a.takePageChanged()

	// The cog, at design (58, 46), is at about (32, 26) on this screen.
	cx, cy := dev(58, 46)
	a.HandleEvent(tap(cx, cy))
	if !a.settingsOpen {
		t.Fatalf("a tap on the cog at device (%d,%d) should open settings", cx, cy)
	}
	// Row 1 of the settings list (design y = 120 + 1.5*100 = 270).
	x, y := dev(500, pages.SettingsRowY(1))
	a.HandleEvent(tap(x, y))
	if a.settingsView.Screen != pages.SettingsPickUnit || a.settingsView.Metric != "sog" {
		t.Errorf("view = %+v, want the speed unit picker", a.settingsView)
	}
	// Back, then close.
	a.HandleEvent(tap(cx, cy))
	a.HandleEvent(tap(cx, cy))
	if a.settingsOpen {
		t.Error("back twice should close settings")
	}

	// The thirds of the screen are the thirds of the device.
	a.SetPage("compass")
	a.takePageChanged()
	a.HandleEvent(tap(560, 400)) // the right third of 600
	if a.currentPage().ID != "nav" {
		t.Errorf("a right-third tap went to %s, want the next page (nav)", a.currentPage().ID)
	}
	a.HandleEvent(tap(300, 400)) // the middle: nothing
	if a.currentPage().ID != "nav" {
		t.Error("the middle third should do nothing")
	}
	a.HandleEvent(tap(40, 400)) // the left third
	if a.currentPage().ID != "compass" {
		t.Errorf("a left-third tap went to %s, want the previous page (compass)", a.currentPage().ID)
	}
}

func TestCompassWidgetsAreTappableOnASmallScreen(t *testing.T) {
	a := smallApp(t)
	a.SetPage("compass")
	a.takePageChanged()
	a.State.ApplyForTest("environment.wind.speedApparent", 6)
	a.State.ApplyForTest("environment.wind.speedTrue", 9)

	// The wind widget: design (60, 1060) is inside its tap area.
	x, y := dev(60, 1060)
	a.HandleEvent(tap(x, y))
	if !a.windTrue {
		t.Errorf("a tap at device (%d,%d) on the wind widget should switch to the true wind", x, y)
	}
	if a.currentPage().ID != "compass" {
		t.Error("the widget tap must not page away")
	}
	// The speed box: design (300, 1300).
	x, y = dev(300, 1300)
	a.HandleEvent(tap(x, y))
	if a.speed != pages.SpeedSTW {
		t.Errorf("a tap at device (%d,%d) on the speed box should cycle to STW, got %v", x, y, a.speed)
	}
}

func TestHeartbeatRegionIsScaledToo(t *testing.T) {
	rec := &regionRecorder{PNG: display.PNG{W: 600, H: 800}}
	a := &App{State: signalk.NewState(), Display: rec}
	a.heartbeat(time.Unix(2000, 0))
	if len(rec.regions) != 1 {
		t.Fatalf("got %d region updates", len(rec.regions))
	}
	r := rec.regions[0]
	want := render.ScaleRect(pages.HeartbeatRect(pages.DesignWidth), 600.0/1072)
	if r.x != want.Min.X || r.y != want.Min.Y || r.img.Bounds().Dx() != want.Dx() || r.img.Bounds().Dy() != want.Dy() {
		t.Errorf("region at (%d,%d) %v, want %v", r.x, r.y, r.img.Bounds(), want)
	}
	// It sits inside the header, which is 94 design units = about 53 pixels deep.
	if r.y+r.img.Bounds().Dy() > 55 || r.x+r.img.Bounds().Dx() > 600 {
		t.Errorf("the dot's region (%d,%d) %v is outside the scaled header", r.x, r.y, r.img.Bounds())
	}
}

func TestHeaderGuardBandIsScaled(t *testing.T) {
	rec := &regionRecorder{PNG: display.PNG{W: 600, H: 800}}
	a := &App{State: signalk.NewState(), Display: rec, HeaderGuardEvery: time.Second}
	a.guardHeader(time.Unix(120020, 0))
	if len(rec.regions) != 1 {
		t.Fatalf("got %d region updates", len(rec.regions))
	}
	r := rec.regions[0]
	want := render.ScaleRect(pages.HeaderBand(pages.DesignWidth), 600.0/1072)
	if r.x != 0 || r.y != 0 || r.img.Bounds().Dx() != 600 || r.img.Bounds().Dy() != want.Dy() {
		t.Errorf("guard repainted %v at (%d,%d), want a %v band across the full width", r.img.Bounds(), r.x, r.y, want)
	}
	if want.Dy() < 50 || want.Dy() > 56 {
		t.Errorf("the header band is %d px tall; expected about 53", want.Dy())
	}
}
