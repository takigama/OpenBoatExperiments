package app

import (
	"path/filepath"
	"testing"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/input"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/units"
)

func tap(x, y int) input.Event { return input.Event{Kind: input.Tap, X: x, Y: y} }

func TestSettingsFlow(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	a := &App{Display: &display.PNG{W: 1072, H: 1448}, UnitsPath: path}
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
	saved, err := units.Load(path)
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
