package app

import (
	"path/filepath"
	"testing"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/input"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func tap(x, y int) input.Event { return input.Event{Kind: input.Tap, X: x, Y: y} }

func TestSettingsFlow(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	a := &App{Display: &display.PNG{W: 1072, H: 1448}, SettingsPath: path}
	a.SetPage("nav")
	a.takePageChanged()

	// The cog sits in the left third, where a tap would otherwise mean
	// "previous page" - opening settings must win.
	a.HandleEvent(tap(30, 40))
	if !a.settingsOpen {
		t.Fatal("tapping the cog should open settings")
	}
	if a.currentPage().ID != "nav" {
		t.Errorf("opening settings changed the page to %s", a.currentPage().ID)
	}
	if !a.takePageChanged() {
		t.Error("opening settings is a whole new picture and should force a full refresh")
	}

	// Row 1 is the first metric (speed over ground): open its unit picker,
	// then choose the first unit, knots.
	a.HandleEvent(tap(500, pages.SettingsRowY(1)))
	if a.settingsView.Screen != pages.SettingsPickUnit || a.settingsView.Metric != "sog" {
		t.Fatalf("view = %+v, want the speed unit picker", a.settingsView)
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(0)))
	if got := a.Units.UnitFor("sog").Symbol; got != "kn" {
		t.Errorf("sog unit = %s, want kn", got)
	}
	if a.settingsView.Screen != pages.SettingsRoot {
		t.Errorf("after choosing, the dialog should return to the list, got %+v", a.settingsView)
	}

	// The choice must have been saved, and be what a restart would load.
	saved, err := settings.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if saved.UnitFor("sog").Symbol != "kn" || saved.UnitFor("depth").Symbol != "m" {
		t.Errorf("saved settings = %+v; want sog=kn, depth untouched", saved)
	}

	// While settings are open, taps on the screen edges are settings taps,
	// not page changes.
	a.HandleEvent(input.Event{Kind: input.SwipeLeft, X: 800, Y: 700})
	a.HandleEvent(tap(1000, 5))
	if a.currentPage().ID != "nav" {
		t.Errorf("page changed to %s while settings were open", a.currentPage().ID)
	}

	// Preset picker: switching preset wipes the override.
	a.HandleEvent(tap(500, pages.SettingsRowY(0)))
	if a.settingsView.Screen != pages.SettingsPickPreset {
		t.Fatalf("view = %+v, want the preset picker", a.settingsView)
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(1))) // imperial
	if got := a.Units.UnitFor("sog").Symbol; got != "mph" {
		t.Errorf("after the imperial preset sog = %s, want mph (override cleared)", got)
	}

	// Back from the list closes settings.
	a.HandleEvent(tap(30, 40))
	if a.settingsOpen {
		t.Error("back from the list should close settings")
	}
	a.HandleEvent(tap(1000, 700)) // right third again pages normally
	if a.currentPage().ID != "compass" {
		t.Errorf("after closing settings, a right-edge tap should page to compass, got %s", a.currentPage().ID)
	}
}

func TestInvertTogglePersistsAndStaysOnTheList(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	a := &App{Display: &display.PNG{W: 1072, H: 1448}, SettingsPath: path}
	a.HandleEvent(tap(30, 40)) // open settings
	a.takePageChanged()

	invertRow := pages.SettingsRowY(len(units.Metrics) + 1)
	a.HandleEvent(tap(500, invertRow))
	if !a.Invert {
		t.Fatal("tapping the invert row should turn inversion on")
	}
	if a.settingsView.Screen != pages.SettingsRoot || !a.settingsOpen {
		t.Errorf("toggling should stay on the list so the change is visible, got open=%v view=%+v", a.settingsOpen, a.settingsView)
	}
	if !a.takePageChanged() {
		t.Error("inverting the whole screen needs a full refresh")
	}
	saved, err := settings.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if !saved.Invert {
		t.Error("the choice must be saved")
	}

	a.HandleEvent(tap(500, invertRow))
	if a.Invert {
		t.Error("tapping again should turn it back off")
	}
	if saved, _ := settings.Load(path); saved.Invert {
		t.Error("turning it off must be saved too")
	}
}

func TestInvertFlipsEveryPixelOfTheFrame(t *testing.T) {
	normal := &App{State: signalk.NewState(), Display: &display.PNG{W: 1072, H: 1448}}
	inverted := &App{State: signalk.NewState(), Display: &display.PNG{W: 1072, H: 1448}, Invert: true}
	now := time.Unix(2000, 0)

	a, err := normal.Frame(now)
	if err != nil {
		t.Fatal(err)
	}
	b, err := inverted.Frame(now)
	if err != nil {
		t.Fatal(err)
	}
	for i := range a.Pix {
		if a.Pix[i] != 255-b.Pix[i] {
			t.Fatalf("pixel %d: normal %d, inverted %d - not complements", i, a.Pix[i], b.Pix[i])
		}
	}
	// Including the settings screens, so the toggle's own effect is visible.
	// The one place they legitimately differ is the invert row itself, which
	// reads "On" in one and "Off" in the other; everything else must still
	// be an exact complement.
	normal.OpenSettings(pages.SettingsView{})
	inverted.OpenSettings(pages.SettingsView{})
	a, _ = normal.Frame(now)
	b, _ = inverted.Frame(now)
	rowMid := pages.SettingsRowY(len(units.Metrics) + 1)
	for y := 0; y < 1448; y++ {
		if y >= rowMid-60 && y < rowMid+60 {
			continue
		}
		for x := 0; x < 1072; x++ {
			if a.Pix[y*a.Stride+x] != 255-b.Pix[y*b.Stride+x] {
				t.Fatalf("settings screen pixel (%d,%d) is not inverted", x, y)
			}
		}
	}
}

func TestInvertAlsoFlipsTheHeartbeatDot(t *testing.T) {
	rec := &regionRecorder{PNG: display.PNG{W: 1072, H: 1448}}
	plain := &App{State: signalk.NewState(), Display: rec}
	plain.heartbeat(time.Unix(2000, 0))
	rec2 := &regionRecorder{PNG: display.PNG{W: 1072, H: 1448}}
	flipped := &App{State: signalk.NewState(), Display: rec2, Invert: true}
	flipped.heartbeat(time.Unix(2000, 0))

	a, b := rec.regions[0].img, rec2.regions[0].img
	for y := a.Bounds().Min.Y; y < a.Bounds().Max.Y; y++ {
		for x := a.Bounds().Min.X; x < a.Bounds().Max.X; x++ {
			if a.GrayAt(x, y).Y != 255-b.GrayAt(x, y).Y {
				t.Fatalf("heartbeat pixel (%d,%d) is not inverted: %d vs %d", x, y, a.GrayAt(x, y).Y, b.GrayAt(x, y).Y)
			}
		}
	}
}

func TestBackFromAPickerReturnsToTheList(t *testing.T) {
	a := &App{Display: &display.PNG{W: 1072, H: 1448}}
	a.HandleEvent(tap(30, 40)) // open
	a.HandleEvent(tap(500, pages.SettingsRowY(0)))
	if a.settingsView.Screen != pages.SettingsPickPreset {
		t.Fatalf("view = %+v", a.settingsView)
	}
	a.HandleEvent(tap(30, 40)) // back
	if !a.settingsOpen || a.settingsView.Screen != pages.SettingsRoot {
		t.Errorf("back from a picker should land on the list, got open=%v view=%+v", a.settingsOpen, a.settingsView)
	}
}

func TestUnitsAreSnapshottedForRendering(t *testing.T) {
	a := &App{Display: &display.PNG{W: 10, H: 10}}
	a.Units.SetUnit("sog", "kn")
	snap := a.unitsNow()
	a.Units.SetUnit("sog", "mph")
	if got := snap.UnitFor("sog").Symbol; got != "kn" {
		t.Errorf("a snapshot changed underneath the renderer: got %s, want kn", got)
	}
}
