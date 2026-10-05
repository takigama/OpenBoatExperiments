package app

import (
	"context"
	"image"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/frontlight"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
)

// counting is a display that counts what it is sent.
type counting struct {
	*display.PNG
	mu           sync.Mutex
	shows, fulls int
}

func (c *counting) Show(img *image.Gray, full bool) (bool, error) {
	c.mu.Lock()
	c.shows++
	if full {
		c.fulls++
	}
	c.mu.Unlock()
	return c.PNG.Show(img, full)
}

func (c *counting) counts() (int, int) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.shows, c.fulls
}

type powerRig struct {
	a     *App
	bat   *battery.Fake
	light *frontlight.Fake
	disp  *counting
	calls *[]bool // OnNoPower, in order
	path  string
}

// newPowerRig is an app with a battery that can be plugged and unplugged, a front
// light at 12 of 24, and the no-power timeout at its default, an hour.
func newPowerRig(t *testing.T, plugged bool) *powerRig {
	t.Helper()
	dir := t.TempDir()
	r := &powerRig{
		bat:   &battery.Fake{S: battery.Status{Percent: 70, Plugged: plugged, Charging: plugged}},
		light: frontlight.NewFake(24, 12),
		disp:  &counting{PNG: &display.PNG{W: 1072, H: 1448, Path: filepath.Join(dir, "f.png")}},
		calls: new([]bool),
		path:  filepath.Join(dir, "settings.json"),
	}
	calls := r.calls
	r.a = &App{
		State: signalk.NewState(), Display: r.disp, Battery: r.bat, Light: r.light,
		NoPowerMin: 60, battEvery: time.Nanosecond, SettingsPath: r.path,
		OnNoPower: func(on bool) { *calls = append(*calls, on) },
	}
	r.a.InitLight()
	r.a.SetPage("nav")
	r.a.takePageChanged()
	return r
}

func (r *powerRig) unpluggedFor(d time.Duration) {
	r.a.mu.Lock()
	r.a.unpluggedSince = time.Now().Add(-d)
	r.a.mu.Unlock()
}

func TestNoPowerComesAfterTheTimeoutOnBattery(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	t0 := time.Now()

	a.powerTick(t0)
	if a.inNoPower() {
		t.Fatal("it was only just unplugged")
	}
	a.powerTick(t0.Add(59 * time.Minute))
	if a.inNoPower() {
		t.Fatal("59 minutes off power is not yet an hour")
	}
	a.powerTick(t0.Add(60 * time.Minute))
	if !a.inNoPower() {
		t.Fatal("an hour off power should go to NO POWER")
	}
	if got := *r.calls; len(got) != 1 || !got[0] {
		t.Errorf("OnNoPower calls = %v, want [true]", got)
	}
	if r.light.Current != 0 {
		t.Errorf("the front light should be off, at %d", r.light.Current)
	}
	if !a.Control().NoPower || a.Control().NoPowerMin != 60 {
		t.Errorf("Control = %+v", a.Control())
	}
	// More ticks change nothing, and tell nobody again.
	a.powerTick(t0.Add(61 * time.Minute))
	a.powerTick(t0.Add(3 * time.Hour))
	if len(*r.calls) != 1 {
		t.Errorf("it kept announcing: %v", *r.calls)
	}

	// The screen is the NO POWER message, not the page.
	img, err := a.Frame(t0)
	if err != nil {
		t.Fatal(err)
	}
	dark := 0
	for y := 400; y < 1000; y += 2 {
		for x := 80; x < 1000; x += 2 {
			if img.GrayAt(x, y).Y < 100 {
				dark++
			}
		}
	}
	if dark < 20000 {
		t.Errorf("NO POWER is not on the screen: %d dark samples", dark)
	}
	if img.GrayAt(60, 40).Y < 200 || img.GrayAt(900, 40).Y < 200 {
		t.Error("the header (clock, battery) should be gone: nothing on it is being kept up to date")
	}
}

func TestPluggingInBringsEverythingBack(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	t0 := time.Now()
	a.powerTick(t0)
	a.powerTick(t0.Add(time.Hour))
	if !a.inNoPower() {
		t.Fatal("setup: it should be in NO POWER")
	}
	a.takePageChanged()

	r.bat.S = battery.Status{Percent: 71, Plugged: true, Charging: true}
	a.powerTick(t0.Add(time.Hour + 30*time.Second))
	if a.inNoPower() {
		t.Fatal("power is back")
	}
	if got := *r.calls; len(got) != 2 || got[1] {
		t.Errorf("OnNoPower calls = %v, want [true false]", got)
	}
	if r.light.Current != 12 || a.Control().Light != 12 {
		t.Errorf("the light should be back at 12: %d / %d", r.light.Current, a.Control().Light)
	}
	if !a.takePageChanged() {
		t.Error("coming back is a whole new picture: a full refresh")
	}
	// Unplugged again later starts a whole new hour.
	r.bat.S = battery.Status{Percent: 70, Plugged: false}
	a.powerTick(t0.Add(2 * time.Hour))
	a.powerTick(t0.Add(2*time.Hour + 59*time.Minute))
	if a.inNoPower() {
		t.Error("the hour should count from when power was lost again")
	}
	a.powerTick(t0.Add(3*time.Hour + time.Minute))
	if !a.inNoPower() {
		t.Error("and an hour on, it should go to NO POWER again")
	}
}

func TestNoPowerNeverHappensWithoutKnowingTheBatteryIsShort(t *testing.T) {
	for name, mutate := range map[string]func(*powerRig){
		"on power":   func(r *powerRig) { r.bat.S.Plugged = true },
		"no reader":  func(r *powerRig) { r.a.Battery = nil },
		"read error": func(r *powerRig) { r.bat.Err = os.ErrNotExist },
		"never":      func(r *powerRig) { r.a.NoPowerMin = 0 },
	} {
		r := newPowerRig(t, false)
		mutate(r)
		r.unpluggedFor(48 * time.Hour)
		for i := 0; i < 3; i++ {
			r.a.powerTick(time.Now().Add(time.Duration(i) * time.Hour))
		}
		if r.a.inNoPower() || len(*r.calls) != 0 {
			t.Errorf("%s: it conserved power it had no reason to", name)
		}
	}
}

func TestTheTimeoutIsWhatWasSet(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	if err := a.SetNoPowerMinutes(1); err != nil {
		t.Fatal(err)
	}
	a.mu.Lock()
	a.lastActivity = time.Time{}
	a.mu.Unlock()
	t0 := time.Now()
	a.powerTick(t0)
	a.powerTick(t0.Add(59 * time.Second))
	if a.inNoPower() {
		t.Error("59 seconds is not a minute")
	}
	a.powerTick(t0.Add(time.Minute))
	if !a.inNoPower() {
		t.Error("as short as a minute should work")
	}
	// Changing it to never, while it is showing, brings it back at once.
	if err := a.SetNoPowerMinutes(0); err != nil {
		t.Fatal(err)
	}
	if a.inNoPower() {
		t.Error("setting it to never must end NO POWER (the setting is also a command, which wakes it)")
	}
	a.powerTick(t0.Add(30 * time.Hour))
	if a.inNoPower() {
		t.Error("never means never")
	}
}

func TestItDoesNotBlankWhileSomeoneIsUsingIt(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	r.unpluggedFor(2 * time.Hour) // well past the timeout
	a.Wake()                      // someone has just touched it
	now := time.Now()
	a.powerTick(now.Add(5 * time.Second))
	if a.inNoPower() {
		t.Error("it blanked five seconds after a touch")
	}
	a.powerTick(now.Add(noPowerIdle + time.Second))
	if !a.inNoPower() {
		t.Error("once it has been quiet for a while it should go")
	}
}

func TestATouchWakesItForAnotherTimeoutAndDoesNothingElse(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	t0 := time.Now()
	a.powerTick(t0)
	a.powerTick(t0.Add(time.Hour))
	if !a.inNoPower() {
		t.Fatal("setup")
	}
	a.takePageChanged()

	a.HandleEvent(tap(1000, 700)) // the right third: would normally page on
	if a.inNoPower() {
		t.Fatal("a tap should wake it")
	}
	if a.currentPage().ID != "nav" {
		t.Errorf("the tap that woke it paged to %s: it should only wake", a.currentPage().ID)
	}
	if r.light.Current != 12 {
		t.Errorf("the light should be back: %d", r.light.Current)
	}
	if got := *r.calls; len(got) != 2 || got[1] {
		t.Errorf("OnNoPower calls = %v", got)
	}
	// The next tap works as usual.
	a.HandleEvent(tap(1000, 700))
	if a.currentPage().ID != "map" {
		t.Errorf("the second tap should page on, got %s", a.currentPage().ID)
	}
	// It waits a whole timeout again from the touch.
	now := time.Now()
	a.powerTick(now.Add(59 * time.Minute))
	if a.inNoPower() {
		t.Error("after a tap it should wait a full hour again")
	}
	a.powerTick(now.Add(time.Hour + time.Minute))
	if !a.inNoPower() {
		t.Error("and then go back to NO POWER")
	}
}

func TestThePowerButtonAndTheWebWakeItToo(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	enter := func() {
		r.unpluggedFor(2 * time.Hour)
		a.mu.Lock()
		a.lastActivity = time.Time{}
		a.mu.Unlock()
		a.powerTick(time.Now())
		if !a.inNoPower() {
			t.Fatal("setup: not in NO POWER")
		}
	}

	enter()
	a.PowerButton()
	if a.inNoPower() {
		t.Error("the power button should wake it")
	}
	if !a.settingsOpen || a.settingsView.Screen != pages.SettingsPower {
		t.Errorf("and open the power screen, got open=%v %+v", a.settingsOpen, a.settingsView)
	}

	a.settingsOpen = false
	enter()
	a.Wake()
	if a.inNoPower() {
		t.Error("Wake should wake it")
	}

	for name, cmd := range map[string]func(){
		"invert":   func() { a.SetInvert(true) },
		"page":     func() { a.ChoosePage("compass") },
		"box":      func() { a.SetBox(0, "depth") },
		"widget":   func() { a.SetSpeed("stw") },
		"map":      func() { a.SetMapNorthUp(true) },
		"light":    func() { a.SetBrightness(3) },
		"units":    func() { a.SetUnits("nautical", nil) },
		"demo":     func() { a.SetDemoMode(false) },
		"timeout":  func() { a.SetNoPowerMinutes(30) },
		"wind":     func() { a.SetWindTrue(true) },
		"depthbox": func() { a.SetDepth("sog") },
	} {
		enter()
		cmd()
		if a.inNoPower() {
			t.Errorf("a %s command should wake it: whoever sent it wants to see the result", name)
		}
	}
}

func TestNoPowerDoesNotInterruptPoweringOff(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	a.OnPower = func(string) error { return nil }
	a.doPower(pages.PowerOff) // the last picture is up
	r.unpluggedFor(5 * time.Hour)
	a.powerTick(time.Now())
	if a.inNoPower() {
		t.Error("once powering off, the last picture must stay")
	}
}

func TestTheLightIsOnlyTouchedIfItWasOn(t *testing.T) {
	r := newPowerRig(t, false)
	if err := r.a.SetBrightness(0); err != nil {
		t.Fatal(err)
	}
	sets := len(r.light.Sets)
	r.unpluggedFor(2 * time.Hour)
	r.a.mu.Lock()
	r.a.lastActivity = time.Time{}
	r.a.mu.Unlock()
	r.a.powerTick(time.Now())
	if !r.a.inNoPower() {
		t.Fatal("setup")
	}
	if len(r.light.Sets) != sets {
		t.Errorf("the light was off already; nothing should have been sent to it: %v", r.light.Sets)
	}
	r.bat.S.Plugged = true
	r.a.powerTick(time.Now())
	if len(r.light.Sets) != sets || r.light.Current != 0 {
		t.Errorf("coming back must not switch on a light that was off: %v, now %d", r.light.Sets, r.light.Current)
	}
	// And the level the user had is not lost: it is still what is saved.
	r2 := newPowerRig(t, false)
	r2.a.SetBrightness(9)
	r2.unpluggedFor(2 * time.Hour)
	r2.a.mu.Lock()
	r2.a.lastActivity = time.Time{}
	r2.a.mu.Unlock()
	r2.a.powerTick(time.Now())
	f, _ := settings.Load(r2.path)
	if f.Brightness == nil || *f.Brightness != 9 {
		t.Errorf("no-power mode must not overwrite the saved light level: %+v", f.Brightness)
	}
}

func TestSetNoPowerMinutes(t *testing.T) {
	r := newPowerRig(t, true)
	a := r.a
	for _, bad := range []int{-1, -60, 10081, 1 << 20} {
		if err := a.SetNoPowerMinutes(bad); err == nil {
			t.Errorf("%d minutes should be refused", bad)
		}
	}
	if a.NoPowerMin != 60 || a.NoPowerChosen {
		t.Error("a refused value must change nothing")
	}
	// An unrelated change is saved without the timeout, which was never chosen.
	a.SetInvert(true)
	if f, _ := settings.Load(r.path); f.NoPowerMinutes != nil {
		t.Errorf("a timeout nobody chose was saved: %d", *f.NoPowerMinutes)
	}
	for _, ok := range []int{0, 1, 90, 10080} {
		if err := a.SetNoPowerMinutes(ok); err != nil {
			t.Fatalf("%d: %v", ok, err)
		}
		if a.Control().NoPowerMin != ok {
			t.Errorf("Control says %d, want %d", a.Control().NoPowerMin, ok)
		}
		f, err := settings.Load(r.path)
		if err != nil || f.NoPowerMinutes == nil || *f.NoPowerMinutes != ok {
			t.Errorf("%d not saved: %+v %v", ok, f.NoPowerMinutes, err)
		}
		if !f.Invert {
			t.Error("saving the timeout lost another setting")
		}
	}
}

func TestChoosingTheTimeoutOnTheScreen(t *testing.T) {
	r := newPowerRig(t, true)
	a := r.a
	a.OpenSettings(pages.SettingsView{Screen: pages.SettingsPower})
	a.HandleEvent(tap(500, 950)) // the No-power mode row
	if a.settingsView.Screen != pages.SettingsNoPower {
		t.Fatalf("the row should open the picker, got %+v", a.settingsView)
	}
	if !a.withLight(a.settingsView).Demo && a.withLight(a.settingsView).NoPower != 60 {
		t.Errorf("the picker should be told the current timeout: %+v", a.withLight(a.settingsView))
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(1))) // 5 minutes
	if a.NoPowerMin != 5 || !a.NoPowerChosen {
		t.Errorf("after choosing 5 minutes: %d chosen=%v", a.NoPowerMin, a.NoPowerChosen)
	}
	if a.settingsView.Screen != pages.SettingsPower {
		t.Errorf("after choosing, back to the power screen, got %+v", a.settingsView)
	}
	if f, _ := settings.Load(r.path); f.NoPowerMinutes == nil || *f.NoPowerMinutes != 5 {
		t.Errorf("the choice should be saved: %+v", f.NoPowerMinutes)
	}
	// Never is the last row.
	a.HandleEvent(tap(500, 950))
	a.HandleEvent(tap(500, pages.SettingsRowY(len(pages.NoPowerChoices)-1)))
	if a.NoPowerMin != 0 {
		t.Errorf("Never should set 0, got %d", a.NoPowerMin)
	}
	if f, _ := settings.Load(r.path); f.NoPowerMinutes == nil || *f.NoPowerMinutes != 0 {
		t.Errorf("never should be saved as an explicit 0 (not 'unset'): %+v", f.NoPowerMinutes)
	}
}

// waitUntil polls for up to 3 seconds.
func waitUntil(t *testing.T, what string, ok func() bool) {
	t.Helper()
	for i := 0; i < 300; i++ {
		if ok() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestTheMainLoopStopsDrawingInNoPowerAndStartsAgainOnPower(t *testing.T) {
	r := newPowerRig(t, false)
	a := r.a
	a.Interval = 20 * time.Millisecond
	a.SetPage("compass")
	r.unpluggedFor(2 * time.Hour)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { a.Run(ctx); close(done) }()
	defer func() { cancel(); <-done }()

	waitUntil(t, "NO POWER", a.inNoPower)
	waitUntil(t, "the NO POWER screen to be drawn", func() bool { n, _ := r.disp.counts(); return n >= 1 })
	time.Sleep(100 * time.Millisecond)
	shows, fulls := r.disp.counts()
	time.Sleep(500 * time.Millisecond)
	later, laterFulls := r.disp.counts()
	if later != shows || laterFulls != fulls {
		t.Errorf("the screen kept updating in NO POWER: %d draws, then %d", shows, later)
	}
	if shows != 1 || fulls != 1 {
		t.Errorf("NO POWER should be one full-refresh picture, got %d draws (%d full)", shows, fulls)
	}

	// Power comes back: the loop notices on its next look and draws the page again.
	r.bat.S = battery.Status{Percent: 72, Plugged: true, Charging: true}
	a.nudge()
	waitUntil(t, "NO POWER to end", func() bool { return !a.inNoPower() })
	waitUntil(t, "the page to be drawn again", func() bool { n, _ := r.disp.counts(); return n > shows })
	if _, f := r.disp.counts(); f < 2 {
		t.Error("coming back should be a full refresh")
	}
	if r.light.Current != 12 {
		t.Errorf("the light should be back at 12, at %d", r.light.Current)
	}
}

func TestPreviewNoPowerDrawsTheScreen(t *testing.T) {
	a := &App{State: signalk.NewState(), Display: &display.PNG{W: 600, H: 800, Path: filepath.Join(t.TempDir(), "f.png")}}
	a.PreviewNoPower()
	img, err := a.Frame(time.Now())
	if err != nil {
		t.Fatal(err)
	}
	dark := 0
	for y := 0; y < 800; y += 2 {
		for x := 0; x < 600; x += 2 {
			if img.GrayAt(x, y).Y < 100 {
				dark++
			}
		}
	}
	if dark < 3000 {
		t.Errorf("the small-screen NO POWER is nearly empty: %d", dark)
	}
	if !strings.Contains(pages.NoPowerLabel(60), "hour") {
		t.Error("labels")
	}
}
