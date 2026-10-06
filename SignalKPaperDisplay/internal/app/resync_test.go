package app

import (
	"testing"
	"time"

	"signalkpaperdisplay/internal/pages"
)

// A SignalK server only says a switch's value when it changes, so turning idle mode on
// (or choosing another switch) while connected has to have the connection remade, or
// the switch stays unknown until it next flips.
func TestChangingTheIdleSwitchHasTheConnectionRemade(t *testing.T) {
	r, calls := newIdleRig(t)
	a := r.a
	resyncs := 0
	a.OnSwitchWatch = func() { resyncs++ }

	if err := a.SetIdleSwitch(true, ""); err != nil {
		t.Fatal(err)
	}
	if resyncs != 1 {
		t.Errorf("turning it on: %d resyncs, want 1", resyncs)
	}
	if err := a.SetIdleSwitch(true, "electrical.switches.bank.kindle.0.state"); err != nil {
		t.Fatal(err)
	}
	if resyncs != 2 {
		t.Errorf("choosing another switch: %d resyncs, want 2", resyncs)
	}
	// Turning it off watches nothing, so there is nothing to ask for.
	if err := a.SetIdleSwitch(false, ""); err != nil {
		t.Fatal(err)
	}
	if resyncs != 2 {
		t.Errorf("turning it off asked again (%d)", resyncs)
	}
	// A refused path changes nothing.
	if err := a.SetIdleSwitch(true, "not a path"); err == nil || resyncs != 2 {
		t.Errorf("a bad path: err %v, resyncs %d", err, resyncs)
	}

	// While idle, leaving it already makes a new connection (OnIdle false): not two.
	if err := a.SetIdleSwitch(true, ""); err != nil {
		t.Fatal(err)
	}
	resyncs = 0
	*calls = nil
	setSwitch(a, false)
	a.idleTick(time.Now().Add(20 * time.Minute))
	if !isIdle(a) {
		t.Fatal("setup: it should be idle")
	}
	if err := a.SetIdleSwitch(true, "electrical.switches.bank.kindle.1.state"); err != nil {
		t.Fatal(err)
	}
	if resyncs != 0 {
		t.Errorf("changing the switch while idle remade the connection twice (%d resyncs)", resyncs)
	}
	if last := (*calls)[len(*calls)-1]; last.on {
		t.Errorf("it should have left idle: %v", *calls)
	}
}

func TestTurningIdleOnFromTheScreenAsksForTheSwitch(t *testing.T) {
	a, _ := newMoreApp(t)
	resyncs := 0
	a.OnSwitchWatch = func() { resyncs++ }
	openMore(a)
	a.HandleEvent(tap(500, pages.SettingsRowY(1)))
	if !a.IdleEnabled || resyncs != 1 {
		t.Errorf("on: enabled %v, resyncs %d", a.IdleEnabled, resyncs)
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(1)))
	if a.IdleEnabled || resyncs != 1 {
		t.Errorf("off: enabled %v, resyncs %d", a.IdleEnabled, resyncs)
	}
}
