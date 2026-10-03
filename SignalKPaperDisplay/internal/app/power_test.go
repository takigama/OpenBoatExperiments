package app

import (
	"errors"
	"path/filepath"
	"testing"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/units"
)

func newPowerApp(t *testing.T, onPower func(string) error) (*App, *[]string) {
	t.Helper()
	var order []string
	a := &App{Display: &display.PNG{W: 1072, H: 1448, Path: filepath.Join(t.TempDir(), "frame.png")}}
	a.OnPower = func(kind string) error {
		order = append(order, "power:"+kind)
		if onPower != nil {
			return onPower(kind)
		}
		return nil
	}
	a.SetPage("nav")
	a.takePageChanged()
	return a, &order
}

func openPower(a *App) {
	a.HandleEvent(tap(30, 40)) // the cog
	// With no front light the power row is the last: after the demo switch.
	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+5)))
}

func TestPowerFlowAsksBeforeDoingAnything(t *testing.T) {
	a, order := newPowerApp(t, nil)
	openPower(a)
	if a.settingsView.Screen != pages.SettingsPower {
		t.Fatalf("the Power row should open the power screen, got %+v", a.settingsView)
	}
	// Pick power off: it asks, and nothing has happened yet.
	var off int
	for i, c := range pages.PowerChoices {
		if c.ID == pages.PowerOff {
			off = i
		}
	}
	rebuild(t, a, order, off)
	if len(*order) != 0 {
		t.Fatalf("choosing must not carry anything out: %v", *order)
	}
}

// rebuild picks choice i on a fresh power screen and checks the confirmation is for it.
func rebuild(t *testing.T, a *App, order *[]string, i int) *App {
	t.Helper()
	a.mu.Lock()
	a.settingsView = pages.SettingsView{Screen: pages.SettingsPower}
	a.mu.Unlock()
	y := 190 + i*(190+40) + 95
	a.HandleEvent(tap(500, y))
	if a.settingsView.Screen != pages.SettingsPowerConfirm || a.settingsView.Power != pages.PowerChoices[i].ID {
		t.Fatalf("picking choice %d should ask about %s, got %+v", i, pages.PowerChoices[i].ID, a.settingsView)
	}
	return a
}

func TestConfirmingCarriesItOutAndLeavesTheLastPicture(t *testing.T) {
	for i, ch := range pages.PowerChoices {
		a, order := newPowerApp(t, nil)
		openPower(a)
		rebuild(t, a, order, i)
		a.HandleEvent(tap(300, 700)) // YES
		if len(*order) != 1 || (*order)[0] != "power:"+ch.ID {
			t.Fatalf("%s: calls = %v", ch.ID, *order)
		}
		a.mu.Lock()
		farewell := a.farewell
		a.mu.Unlock()
		if farewell != ch.ID {
			t.Errorf("%s: the farewell picture should be showing, got %q", ch.ID, farewell)
		}
		img, err := a.Frame(time.Now())
		if err != nil {
			t.Fatal(err)
		}
		// The page is gone: the farewell replaced it (no header bar at the top).
		dark := 0
		for x := 0; x < 1072; x++ {
			if img.GrayAt(x, 40).Y < 128 {
				dark++
			}
		}
		if dark != 0 {
			t.Errorf("%s: the last picture should have no header, %d dark pixels at y=40", ch.ID, dark)
		}
	}
}

func TestCancelGoesBackWithoutDoingAnything(t *testing.T) {
	a, order := newPowerApp(t, nil)
	openPower(a)
	rebuild(t, a, order, 2)
	a.HandleEvent(tap(800, 700)) // CANCEL
	if len(*order) != 0 {
		t.Errorf("cancel carried something out: %v", *order)
	}
	if a.settingsView.Screen != pages.SettingsPower {
		t.Errorf("cancel should go back to the choices, got %+v", a.settingsView)
	}
	a.HandleEvent(tap(30, 40)) // the chevron: up to the list
	if a.settingsView.Screen != pages.SettingsRoot {
		t.Errorf("back should reach the list, got %+v", a.settingsView)
	}
}

func TestAFailedActionBringsTheDashboardBack(t *testing.T) {
	a, order := newPowerApp(t, func(string) error { return errors.New("no such command") })
	openPower(a)
	rebuild(t, a, order, 1)
	a.HandleEvent(tap(300, 700))
	if len(*order) != 1 {
		t.Fatalf("it should have been tried: %v", *order)
	}
	a.mu.Lock()
	farewell, open, screen := a.farewell, a.settingsOpen, a.settingsView.Screen
	a.mu.Unlock()
	if farewell != "" {
		t.Errorf("the screen must not stay saying %q when nothing happened", farewell)
	}
	if !open || screen != pages.SettingsPower {
		t.Errorf("after a failure the power screen should be back, got open=%v %v", open, screen)
	}
}

func TestNothingToDoOnADisplayWithoutPower(t *testing.T) {
	a := &App{Display: &display.PNG{W: 1072, H: 1448}} // no OnPower: a PC preview
	a.SetPage("nav")
	openPower(a)
	rebuild(t, a, new([]string), 2)
	a.HandleEvent(tap(300, 700))
	a.mu.Lock()
	farewell := a.farewell
	a.mu.Unlock()
	if farewell != "" {
		t.Errorf("a preview must not claim to be switching off: %q", farewell)
	}
}

func TestPowerButtonOpensAndClosesThePowerScreen(t *testing.T) {
	a, _ := newPowerApp(t, nil)
	a.PowerButton()
	a.mu.Lock()
	open, screen := a.settingsOpen, a.settingsView.Screen
	a.mu.Unlock()
	if !open || screen != pages.SettingsPower {
		t.Fatalf("the button should open the power screen from a page, got open=%v %v", open, screen)
	}
	if !a.takePageChanged() {
		t.Error("a new screen is a full refresh")
	}
	a.PowerButton()
	a.mu.Lock()
	open = a.settingsOpen
	a.mu.Unlock()
	if open {
		t.Error("pressed again, it should close the power screen")
	}

	// From the middle of the settings, it goes to the power screen, not away.
	a.HandleEvent(tap(30, 40))
	a.HandleEvent(tap(500, pages.SettingsRowY(1))) // a unit picker
	a.PowerButton()
	if a.settingsView.Screen != pages.SettingsPower {
		t.Errorf("from inside settings the button should show the power screen, got %+v", a.settingsView)
	}
	// And while it is asking to confirm, the button backs out.
	rebuild(t, a, new([]string), 2)
	a.PowerButton()
	if a.settingsOpen {
		t.Error("the button at the confirmation should close it, not confirm it")
	}
}

func TestPowerButtonIsIgnoredWhileGoingDown(t *testing.T) {
	a, order := newPowerApp(t, nil)
	openPower(a)
	rebuild(t, a, order, 2)
	a.HandleEvent(tap(300, 700))
	a.PowerButton()
	a.mu.Lock()
	open := a.settingsOpen
	a.mu.Unlock()
	if open {
		t.Error("once powering off, a press must not bring a menu over the last picture")
	}
}
