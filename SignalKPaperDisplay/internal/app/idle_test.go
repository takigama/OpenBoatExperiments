package app

import (
	"context"
	"testing"
	"time"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/settings"
)

const swPath = settings.DefaultIdlePath

type idleCall struct {
	on   bool
	path string
}

// newIdleRig is a plugged-in app (so no-power mode stays out of it) with a front
// light at 12 of 24 and idle mode on, watching the default switch.
func newIdleRig(t *testing.T) (*powerRig, *[]idleCall) {
	t.Helper()
	r := newPowerRig(t, true)
	calls := new([]idleCall)
	r.a.IdleEnabled = true
	r.a.OnIdle = func(on bool, path string) { *calls = append(*calls, idleCall{on, path}) }
	r.a.ApplyIdle()
	return r, calls
}

func setSwitch(a *App, on bool) {
	v := 0.0
	if on {
		v = 1
	}
	a.State.ApplyForTest(swPath, v)
}

func isIdle(a *App) bool { return a.Control().Idle }

func TestIdleBeginsWhenTheSwitchGoesOffAndEndsWhenItComesOn(t *testing.T) {
	r, calls := newIdleRig(t)
	a := r.a
	t0 := time.Now()

	a.idleTick(t0)
	if isIdle(a) {
		t.Fatal("the switch has said nothing: it must not idle")
	}
	setSwitch(a, true)
	a.idleTick(t0)
	if isIdle(a) {
		t.Fatal("the switch is on")
	}

	setSwitch(a, false)
	a.idleTick(t0)
	if !isIdle(a) {
		t.Fatal("the switch is off: it should idle")
	}
	if len(*calls) != 1 || !(*calls)[0].on || (*calls)[0].path != swPath {
		t.Errorf("OnIdle calls = %v, want [{true %s}]", *calls, swPath)
	}
	if r.light.Current != 0 {
		t.Errorf("the light should be off, at %d", r.light.Current)
	}
	if c := a.Control(); !c.Idle || c.IdleSwitch != "off" || !c.IdleEnabled || c.IdlePath != swPath {
		t.Errorf("Control = %+v", c)
	}
	// Ticking on changes nothing and tells nobody again.
	a.idleTick(t0.Add(time.Minute))
	a.idleTick(t0.Add(time.Hour))
	if len(*calls) != 1 {
		t.Errorf("it kept announcing: %v", *calls)
	}
	if !a.sleeping() {
		t.Error("idle is a sleeping state")
	}

	// The screen is the IDLE picture, not the page.
	idleFrame, err := a.Frame(t0)
	if err != nil {
		t.Fatal(err)
	}
	np := &App{State: a.State, Display: a.Display}
	np.PreviewNoPower()
	npFrame, _ := np.Frame(t0)
	if string(idleFrame.Pix) == string(npFrame.Pix) {
		t.Error("the idle screen is the NO POWER screen")
	}
	if idleFrame.GrayAt(60, 40).Y < 200 || idleFrame.GrayAt(900, 40).Y < 200 {
		t.Error("the header should be gone: nothing on it is being kept up to date")
	}

	// On again: the light, the connection and the page come back.
	a.takePageChanged()
	setSwitch(a, true)
	a.idleTick(t0.Add(2 * time.Hour))
	if isIdle(a) {
		t.Fatal("the switch is on again")
	}
	if len(*calls) != 2 || (*calls)[1].on {
		t.Errorf("OnIdle calls = %v, want [{true} {false}]", *calls)
	}
	if r.light.Current != 12 || a.Control().Light != 12 {
		t.Errorf("the light should be back at 12: %d / %d", r.light.Current, a.Control().Light)
	}
	if !a.takePageChanged() {
		t.Error("coming back is a whole new picture: a full refresh")
	}
}

func TestIdleNeverHappensWithoutReason(t *testing.T) {
	for name, mutate := range map[string]func(*App){
		"turned off":     func(a *App) { a.IdleEnabled = false },
		"the demo is on": func(a *App) { a.Demo = true },
	} {
		r, calls := newIdleRig(t)
		setSwitch(r.a, false)
		mutate(r.a)
		r.a.idleTick(time.Now())
		r.a.idleTick(time.Now().Add(time.Hour))
		if isIdle(r.a) || len(*calls) != 0 {
			t.Errorf("%s: it idled anyway", name)
		}
	}

	// A switch that has not been heard from is not an off switch.
	r, calls := newIdleRig(t)
	r.a.idleTick(time.Now().Add(time.Hour))
	if isIdle(r.a) || len(*calls) != 0 {
		t.Error("an unknown switch put it to sleep")
	}
	// Nor is one that was heard from a server that has since been replaced.
	setSwitch(r.a, true)
	r.a.State.Reset()
	r.a.idleTick(time.Now())
	if isIdle(r.a) {
		t.Error("a forgotten value idled it")
	}
}

func TestIdleWaitsForQuietAfterATouch(t *testing.T) {
	r, _ := newIdleRig(t)
	a := r.a
	now := time.Now()
	a.mu.Lock()
	a.lastActivity = now
	a.mu.Unlock()
	setSwitch(a, false)
	a.idleTick(now.Add(10 * time.Second))
	if isIdle(a) {
		t.Fatal("someone touched it 10 seconds ago: it must not blank under their finger")
	}
	a.idleTick(now.Add(31 * time.Second))
	if !isIdle(a) {
		t.Error("30 quiet seconds later it should idle")
	}
}

func TestATapWakesItAndHoldsIdleOffForAWhile(t *testing.T) {
	r, calls := newIdleRig(t)
	a := r.a
	a.SetPage("nav")
	t0 := time.Now()
	setSwitch(a, false)
	a.idleTick(t0)
	if !isIdle(a) {
		t.Fatal("setup: it should be idle")
	}
	a.takePageChanged()

	a.HandleEvent(tap(540, 700)) // the middle: would do nothing on a page
	if isIdle(a) {
		t.Fatal("a tap should wake it")
	}
	if a.currentPage().ID != "nav" {
		t.Errorf("the waking tap did something else: page is %s", a.currentPage().ID)
	}
	if len(*calls) != 2 || (*calls)[1].on {
		t.Errorf("OnIdle calls = %v", *calls)
	}
	if !a.takePageChanged() || r.light.Current != 12 {
		t.Error("it should come back whole, with the light")
	}

	// The switch is still off: it stays awake for a while, then goes back.
	a.idleTick(time.Now().Add(time.Minute))
	a.idleTick(time.Now().Add(4 * time.Minute))
	if isIdle(a) {
		t.Fatal("it went straight back to sleep although someone had just woken it")
	}
	a.idleTick(time.Now().Add(6 * time.Minute))
	if !isIdle(a) {
		t.Error("five quiet minutes on, with the switch still off, it should idle again")
	}
}

func TestFlippingTheSwitchStartsAfreshAfterAWake(t *testing.T) {
	r, _ := newIdleRig(t)
	a := r.a
	t0 := time.Now()
	setSwitch(a, false)
	a.idleTick(t0)
	a.Wake() // by hand: holds idle off for five minutes
	if isIdle(a) {
		t.Fatal("setup")
	}
	a.idleTick(t0.Add(time.Second)) // still held

	setSwitch(a, true) // the boat says "wake up"...
	a.idleTick(t0.Add(2 * time.Second))
	setSwitch(a, false) // ...and then "sleep" again: that is news, not the old hold
	a.idleTick(t0.Add(3 * time.Second))
	a.idleTick(t0.Add(40 * time.Second))
	if !isIdle(a) {
		t.Error("a fresh off should idle after the usual 30 quiet seconds, not wait out the old hold")
	}
}

func TestWebCommandsWakeItToo(t *testing.T) {
	r, _ := newIdleRig(t)
	a := r.a
	setSwitch(a, false)
	a.idleTick(time.Now())
	if !isIdle(a) {
		t.Fatal("setup")
	}
	if err := a.ChoosePage("compass"); err != nil || isIdle(a) {
		t.Errorf("a command from the web page should wake it (err %v)", err)
	}
}

func TestIdleAndNoPowerShareTheLight(t *testing.T) {
	// Idle first, then the battery runs out: the light was noted once, and comes
	// back only when the last of them ends.
	r, _ := newIdleRig(t)
	a := r.a
	t0 := time.Now()
	setSwitch(a, false)
	a.idleTick(t0)
	if !isIdle(a) || r.light.Current != 0 {
		t.Fatal("setup")
	}
	r.bat.S = battery.Status{Percent: 60, Plugged: false}
	a.powerTick(t0)
	a.powerTick(t0.Add(time.Hour))
	if !a.inNoPower() {
		t.Fatal("setup: no-power after an hour on battery")
	}
	r.bat.S = battery.Status{Percent: 61, Plugged: true, Charging: true}
	a.powerTick(t0.Add(time.Hour + time.Minute))
	if a.inNoPower() {
		t.Fatal("power is back")
	}
	if !isIdle(a) || r.light.Current != 0 {
		t.Errorf("idle is still on, so the light stays off (idle %v, light %d)", isIdle(a), r.light.Current)
	}
	setSwitch(a, true)
	a.idleTick(t0.Add(time.Hour + 2*time.Minute))
	if isIdle(a) || r.light.Current != 12 {
		t.Errorf("both are over: the light should be back at 12, at %d (idle %v)", r.light.Current, isIdle(a))
	}

	// The other way round: no-power first; idle waits for it to end.
	r2, calls2 := newIdleRig(t)
	a2 := r2.a
	r2.bat.S = battery.Status{Percent: 60, Plugged: false}
	a2.powerTick(t0)
	a2.powerTick(t0.Add(time.Hour))
	if !a2.inNoPower() {
		t.Fatal("setup 2")
	}
	setSwitch(a2, false)
	a2.idleTick(t0.Add(time.Hour))
	if isIdle(a2) || len(*calls2) != 0 {
		t.Error("idle began while the screen was already on NO POWER")
	}
	r2.bat.S = battery.Status{Percent: 61, Plugged: true, Charging: true}
	a2.powerTick(t0.Add(time.Hour + time.Minute))
	a2.idleTick(t0.Add(time.Hour + time.Minute))
	if !isIdle(a2) {
		t.Error("power is back and the switch is still off: it should now idle")
	}
	setSwitch(a2, true)
	a2.idleTick(t0.Add(2 * time.Hour))
	if r2.light.Current != 12 {
		t.Errorf("the light should end at 12, at %d", r2.light.Current)
	}
}

func TestChangingTheIdleSettingsAppliesAndSaves(t *testing.T) {
	r, calls := newIdleRig(t)
	a := r.a
	setSwitch(a, false)
	a.idleTick(time.Now())
	if !isIdle(a) {
		t.Fatal("setup")
	}

	// A path that is not a path changes nothing.
	if err := a.SetIdleSwitch(true, "no spaces allowed"); err == nil {
		t.Error("a bad path was accepted")
	}
	if !isIdle(a) || a.Control().IdlePath != swPath {
		t.Error("a refused change changed something")
	}

	// A different switch: the old one's "off" means nothing for it.
	if err := a.SetIdleSwitch(true, "electrical.switches.nav.state"); err != nil {
		t.Fatal(err)
	}
	if isIdle(a) {
		t.Error("choosing another switch should wake it: the new one has said nothing yet")
	}
	c := a.Control()
	if c.IdlePath != "electrical.switches.nav.state" || c.IdleChosen != "electrical.switches.nav.state" || c.IdleSwitch != "unknown" {
		t.Errorf("Control = %+v", c)
	}
	f, err := settings.Load(r.path)
	if err != nil {
		t.Fatal(err)
	}
	if !f.IdleEnabled || f.IdlePath != "electrical.switches.nav.state" {
		t.Errorf("saved %+v", f)
	}
	a.State.ApplyForTest("electrical.switches.nav.state", 0)
	a.idleTick(time.Now().Add(10 * time.Minute)) // changing a setting is a touch: wait out its quiet
	if !isIdle(a) {
		t.Error("the new switch is off: it should idle")
	}
	if last := (*calls)[len(*calls)-1]; !last.on || last.path != "electrical.switches.nav.state" {
		t.Errorf("OnIdle was told %v: it must watch the new switch", last)
	}

	// Back to the default: saved as "no choice", not as the default's name.
	if err := a.SetIdleSwitch(true, swPath); err != nil {
		t.Fatal(err)
	}
	if f, _ := settings.Load(r.path); f.IdlePath != "" {
		t.Errorf("the default is not a choice, but %q was saved", f.IdlePath)
	}
	if a.Control().IdleChosen != "" || a.Control().IdlePath != swPath {
		t.Errorf("Control = %+v", a.Control())
	}

	// Off: nothing watched, and it wakes.
	setSwitch(a, false)
	a.idleTick(time.Now().Add(20 * time.Minute))
	if !isIdle(a) {
		t.Fatal("setup 2")
	}
	if err := a.SetIdleSwitch(false, ""); err != nil {
		t.Fatal(err)
	}
	if isIdle(a) {
		t.Error("turning idle mode off should wake it")
	}
	if _, known, _ := a.State.Switch(); known {
		t.Error("with idle mode off no switch should be watched")
	}
	if f, _ := settings.Load(r.path); f.IdleEnabled {
		t.Error("off was not saved")
	}
}

func TestTheMainLoopDrawsIdleOnceAndStopsThere(t *testing.T) {
	r, _ := newIdleRig(t)
	a := r.a
	a.Interval = 20 * time.Millisecond
	a.SetPage("compass")

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { a.Run(ctx); close(done) }()
	defer func() { cancel(); <-done }()

	waitUntil(t, "the first page", func() bool { n, _ := r.disp.counts(); return n >= 1 })
	setSwitch(a, false)
	a.nudge()
	waitUntil(t, "idle", func() bool { return isIdle(a) })
	var shows, fulls int
	waitUntil(t, "the IDLE screen, in full", func() bool {
		shows, fulls = r.disp.counts()
		return fulls >= 2
	})
	time.Sleep(100 * time.Millisecond)
	shows, fulls = r.disp.counts()
	time.Sleep(500 * time.Millisecond)
	later, laterFulls := r.disp.counts()
	if later != shows || laterFulls != fulls {
		t.Errorf("the screen kept updating while idle: %d draws, then %d", shows, later)
	}

	setSwitch(a, true)
	a.nudge()
	waitUntil(t, "idle to end", func() bool { return !isIdle(a) })
	waitUntil(t, "the page to be drawn again", func() bool { n, _ := r.disp.counts(); return n > shows })
	if _, f := r.disp.counts(); f < laterFulls+1 {
		t.Error("coming back should be a full refresh")
	}
}

func TestPreviewIdleDrawsTheScreen(t *testing.T) {
	r, _ := newIdleRig(t)
	r.a.PreviewIdle()
	img, err := r.a.Frame(time.Now())
	if err != nil {
		t.Fatal(err)
	}
	dark := 0
	for y := 0; y < 1448; y += 2 {
		for x := 0; x < 1072; x += 2 {
			if img.GrayAt(x, y).Y < 100 {
				dark++
			}
		}
	}
	if dark < 5000 {
		t.Errorf("the IDLE screen is nearly empty: %d", dark)
	}
}
