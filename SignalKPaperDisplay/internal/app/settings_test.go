package app

import (
	"os"
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

func TestChoosingWhatANavBoxShows(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	a := &App{State: signalk.NewState(), Display: &display.PNG{W: 1072, H: 1448}, SettingsPath: path,
		Units: units.Settings{Preset: units.PresetMetric}}
	a.HandleEvent(tap(30, 40)) // open settings

	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+2)))
	if a.settingsView.Screen != pages.SettingsBoxes {
		t.Fatalf("view = %+v, want the box list", a.settingsView)
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(3))) // middle right
	if a.settingsView.Screen != pages.SettingsPickBox || a.settingsView.Box != 3 {
		t.Fatalf("view = %+v, want the picker for box 3", a.settingsView)
	}

	// Back from the picker returns to the box list, not the root.
	a.HandleEvent(tap(30, 40))
	if a.settingsView.Screen != pages.SettingsBoxes {
		t.Fatalf("back from the picker landed on %+v", a.settingsView)
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(3)))

	// Choose "Waypoint distance": kind index 13, so row 6, left column.
	idx := -1
	for i, k := range pages.BoxKinds {
		if k.ID == "wpdist" {
			idx = i
		}
	}
	x := 200
	if idx%2 == 1 {
		x = 800
	}
	a.HandleEvent(tap(x, pages.SettingsRowY(idx/2)))
	if got := a.boxesNow()[3]; got != "wpdist" {
		t.Fatalf("box 3 = %q, want wpdist", got)
	}
	if a.settingsView.Screen != pages.SettingsBoxes {
		t.Errorf("after choosing, the dialog should return to the box list, got %+v", a.settingsView)
	}
	if got := a.boxesNow()[0]; got != "sog" {
		t.Errorf("box 0 changed to %q", got)
	}

	saved, err := settings.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(saved.Boxes) != pages.NavBoxes || saved.Boxes[3] != "wpdist" {
		t.Errorf("saved boxes = %v", saved.Boxes)
	}
	// Unit choices and the invert flag survive alongside it.
	if saved.Preset == "" {
		t.Error("saving boxes lost the unit preset")
	}
}

func TestBoxesRoundTripThroughTheFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	f := settings.File{Settings: units.Settings{Preset: units.PresetNautical}, Boxes: []string{"stw", "awa"}}
	if err := settings.Save(path, f); err != nil {
		t.Fatal(err)
	}
	got, err := settings.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(got.Boxes) != 2 || got.Boxes[1] != "awa" || got.Preset != units.PresetNautical {
		t.Errorf("round trip = %+v", got)
	}
	clone := got.Clone()
	clone.Boxes[0] = "changed"
	if got.Boxes[0] == "changed" {
		t.Error("Clone shares the boxes slice")
	}
	// An old file without any boxes still loads, with the defaults applied later.
	old := filepath.Join(t.TempDir(), "old.json")
	if err := os.WriteFile(old, []byte(`{"preset":"metric"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if f, err := settings.Load(old); err != nil || f.Boxes != nil {
		t.Errorf("old file: %+v %v", f, err)
	}
}

// pressKey taps one keypad key: a character, "back", "clear" or "save".
func pressKey(a *App, key string) {
	pos := map[string][2]int{"1": {0, 0}, "2": {0, 1}, "3": {0, 2}, "4": {1, 0}, "5": {1, 1}, "6": {1, 2},
		"7": {2, 0}, "8": {2, 1}, "9": {2, 2}, ".": {3, 0}, "0": {3, 1}, ":": {3, 2},
		"back": {4, 0}, "clear": {4, 1}, "save": {4, 2}}
	p := pos[key]
	a.HandleEvent(tap(p[1]*1072/3+1072/6, 380+p[0]*180+90))
}

// typeKeys taps the keypad keys for each character of text.
func typeKeys(a *App, text string) {
	for _, ch := range text {
		pressKey(a, string(ch))
	}
}

func TestChangingTheSignalKServer(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	var moved []string
	a := &App{State: signalk.NewState(), Display: &display.PNG{W: 1072, H: 1448}, SettingsPath: path,
		Units: units.Settings{Preset: units.PresetMetric}, DefaultServer: "10.0.0.76:3001",
		OnServerChange: func(h string) { moved = append(moved, h) }}
	a.HandleEvent(tap(30, 40))
	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+3)))
	if a.settingsView.Screen != pages.SettingsServer || a.settingsView.Text != "10.0.0.76:3001" {
		t.Fatalf("view = %+v, want the editor showing the current server", a.settingsView)
	}

	// Edit the last number: 3001 -> 3000 by backspace and typing.
	pressKey(a, "back")
	pressKey(a, "back")
	typeKeys(a, "0")
	typeKeys(a, "0")
	if a.settingsView.Text != "10.0.0.76:3000" {
		t.Fatalf("text = %q", a.settingsView.Text)
	}

	// A bad address is refused with a reason and nothing changes.
	pressKey(a, "clear")
	typeKeys(a, "300.1.1.1")
	pressKey(a, "save")
	if a.settingsView.Screen != pages.SettingsServer || a.settingsView.Err == "" {
		t.Fatalf("a bad address should stay in the editor with an error, got %+v", a.settingsView)
	}
	if len(moved) != 0 || a.Server != "" {
		t.Errorf("a refused address changed the server: %q %v", a.Server, moved)
	}
	if _, err := os.Stat(path); err == nil {
		t.Error("a refused address must not be saved")
	}
	// Typing again clears the complaint.
	pressKey(a, "back")
	if a.settingsView.Err != "" {
		t.Error("the error should go once you edit")
	}

	// A good one: saved, applied, persisted, back to the list.
	pressKey(a, "clear")
	typeKeys(a, "192.168.4.2")
	pressKey(a, "save")
	if a.settingsView.Screen != pages.SettingsRoot {
		t.Errorf("after saving the dialog should return to the list, got %+v", a.settingsView)
	}
	if a.Server != "192.168.4.2:3000" {
		t.Errorf("server = %q, want the port defaulted", a.Server)
	}
	if len(moved) != 1 || moved[0] != "192.168.4.2:3000" {
		t.Errorf("the connection should have been moved once: %v", moved)
	}
	saved, err := settings.Load(path)
	if err != nil || saved.Server != "192.168.4.2:3000" {
		t.Errorf("saved = %+v, %v", saved, err)
	}

	// Saving the same address again isn't a change.
	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+3)))
	pressKey(a, "save")
	if len(moved) != 1 {
		t.Errorf("re-saving the same server reconnected: %v", moved)
	}

	// Back from the editor discards what was typed.
	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+3)))
	pressKey(a, "clear")
	typeKeys(a, "9.9.9.9")
	a.HandleEvent(tap(30, 40))
	if a.settingsView.Screen != pages.SettingsRoot || a.Server != "192.168.4.2:3000" {
		t.Errorf("back should discard: view %+v server %q", a.settingsView, a.Server)
	}
}

func TestSavingOtherSettingsDoesNotPinTheDefaultServer(t *testing.T) {
	// With no server chosen, the launcher's address applies. Saving some other
	// setting must not write that address into settings.json, or later changes
	// to the launcher's config would be ignored.
	path := filepath.Join(t.TempDir(), "settings.json")
	a := &App{State: signalk.NewState(), Display: &display.PNG{W: 1072, H: 1448}, SettingsPath: path,
		Units: units.Settings{Preset: units.PresetMetric}, DefaultServer: "10.0.0.76:3001"}
	a.HandleEvent(tap(30, 40))
	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+1))) // invert
	saved, err := settings.Load(path)
	if err != nil || !saved.Invert {
		t.Fatalf("saved = %+v, %v", saved, err)
	}
	if saved.Server != "" {
		t.Errorf("the default server was pinned into the file: %q", saved.Server)
	}
	if got := a.serverNow(); got != "10.0.0.76:3001" {
		t.Errorf("effective server = %q", got)
	}
}
