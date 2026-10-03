package app

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"signalkpaperdisplay/internal/demo"
	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func TestDemoSwitchInSettings(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	var calls []bool
	a := &App{Display: &display.PNG{W: 1072, H: 1448}, SettingsPath: path,
		OnDemoChange: func(on bool) { calls = append(calls, on) }}
	a.SetPage("nav")
	a.HandleEvent(tap(30, 40)) // the cog: open settings
	if !a.settingsOpen {
		t.Fatal("settings did not open")
	}

	// No light on this app, so the demo switch is the row after the server's.
	row := pages.SettingsRowY(len(units.Metrics) + 4)
	a.HandleEvent(tap(500, row))
	if !a.Demo || len(calls) != 1 || !calls[0] {
		t.Fatalf("after one tap: Demo=%v, calls=%v; want on, [true]", a.Demo, calls)
	}
	if !a.settingsOpen || a.settingsView.Screen != pages.SettingsRoot {
		t.Errorf("switching it should leave the list showing, got %+v open=%v", a.settingsView, a.settingsOpen)
	}
	if !a.withLight(a.settingsView).Demo {
		t.Error("the list should be told demo mode is on, to say so")
	}
	a.HandleEvent(tap(500, row))
	if a.Demo || len(calls) != 2 || calls[1] {
		t.Fatalf("after two taps: Demo=%v, calls=%v; want off, [true false]", a.Demo, calls)
	}

	// It is never saved: a restart must go back to the real server. Nothing else
	// changed either, so no settings file was written at all.
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		b, _ := os.ReadFile(path)
		t.Errorf("switching demo mode wrote %s: %s", path, b)
	}
}

func TestDemoSwitchWithoutACallbackIsHarmless(t *testing.T) {
	a := &App{Display: &display.PNG{W: 1072, H: 1448}}
	a.SetPage("nav")
	a.HandleEvent(tap(30, 40))
	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+4)))
	if !a.Demo {
		t.Error("the switch should still flip")
	}
}

func TestEveryPageSaysDemoWhileItIsOn(t *testing.T) {
	st := signalk.NewState()
	st.SetDemo(true)
	for _, m := range demo.New().Tick(0, 1) {
		st.FeedDemo(m, time.Now())
	}
	a := &App{State: st, Display: &display.PNG{W: 1072, H: 1448}}
	for _, p := range pages.All() {
		a.SetPage(p.ID)
		a.Demo = false
		off, err := a.Frame(time.Now())
		if err != nil {
			t.Fatal(err)
		}
		a.Demo = true
		on, err := a.Frame(time.Now())
		if err != nil {
			t.Fatal(err)
		}
		tag := pages.DemoTag
		inked := func(pix func(x, y int) uint8) int {
			n := 0
			for y := tag.Min.Y; y < tag.Max.Y; y++ {
				for x := tag.Min.X; x < tag.Max.X; x++ {
					if pix(x, y) < 128 {
						n++
					}
				}
			}
			return n
		}
		before := inked(func(x, y int) uint8 { return off.GrayAt(x, y).Y })
		after := inked(func(x, y int) uint8 { return on.GrayAt(x, y).Y })
		if after < tag.Dx()*tag.Dy()/2 || after <= before {
			t.Errorf("page %s: the DEMO tag is missing (dark pixels %d without, %d with)", p.ID, before, after)
		}
	}
}
