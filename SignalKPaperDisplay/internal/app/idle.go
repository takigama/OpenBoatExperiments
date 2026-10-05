package app

import (
	"fmt"
	"log"
	"time"

	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
)

// Idle mode. A SignalK switch - electrical.switches.kindle.state, say, flipped by
// whatever the boat uses to say "nobody is looking at this" - puts the dashboard to
// sleep: while it is off the screen shows one IDLE picture and nothing is redrawn,
// the front light is off, and the connection to the server is cut down to that one
// switch (OnIdle tells main to). Turning the switch on wakes it, and so does a tap,
// the power button or a command from the web page.
//
// A wake by hand holds idle mode off for a while (idleRewake), since the switch is
// still off and would otherwise put the screen straight back to sleep; after that
// quiet it goes back. Flipping the switch on and off again starts it afresh.
//
// It is the same sleep as no-power mode, with a different reason, so the two share
// the screen loop (sleeping, showSleep) and the front light's note of where it was.

const (
	// idlePoll is how often, while idle, the switch is looked at. A change arrives
	// on the connection at once, but the main loop only notices when it next wakes.
	idlePoll = 3 * time.Second
	// idleRewake is how long a wake by hand holds idle mode off.
	idleRewake = 5 * time.Minute
)

// sleeping reports whether the screen is on one of its sleeping pictures.
func (a *App) sleeping() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.noPower || a.idle
}

// sleepKind names the sleeping picture that should be showing, "" when none.
func (a *App) sleepKindLocked() string {
	switch {
	case a.noPower:
		return "nopower"
	case a.idle:
		return "idle"
	}
	return ""
}

// sleepPoll is how long the main loop may sleep while the screen is asleep: no
// longer than it takes to notice the way out.
func (a *App) sleepPoll() time.Duration {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.noPower { // the way out is power coming back, which costs a battery read to see
		return noPowerPoll
	}
	return idlePoll
}

// showSleep draws the sleeping picture once, in full, the first time it is called
// after going to sleep (or after changing from one to the other). The caller is the
// main loop.
func (a *App) showSleep(now time.Time) {
	a.mu.Lock()
	kind := a.sleepKindLocked()
	shown := a.sleepDrawn == kind
	a.sleepDrawn = kind
	a.mu.Unlock()
	if shown {
		return
	}
	if _, err := a.Show(now, true); err != nil {
		log.Printf("display: %v", err)
	}
}

// idlePathLocked is the switch idle mode watches. The caller holds a.mu.
func (a *App) idlePathLocked() string {
	if a.IdlePath == "" {
		return settings.DefaultIdlePath
	}
	return a.IdlePath
}

// ApplyIdle tells the state which switch to keep the value of, or none if idle mode
// is off, and drops out of idle if it was on: what the old switch said means nothing
// for the new one. Call it once after setting IdleEnabled and IdlePath; the setters
// call it themselves.
func (a *App) ApplyIdle() {
	a.mu.Lock()
	enabled, path := a.IdleEnabled, a.idlePathLocked()
	a.mu.Unlock()
	if a.State != nil {
		if enabled {
			a.State.SetSwitchPath(path)
		} else {
			a.State.SetSwitchPath("")
		}
	}
	a.exitIdle(time.Now(), false)
}

// idleSettingChanged is ApplyIdle for after the user changed the choice.
func (a *App) idleSettingChanged() {
	a.ApplyIdle()
	a.mu.Lock()
	enabled, path := a.IdleEnabled, a.idlePathLocked()
	a.mu.Unlock()
	log.Printf("settings: idle mode is now %s (%s)", map[bool]string{true: "on", false: "off"}[enabled], path)
	a.nudge()
}

// idleTick looks at the switch and moves in or out of idle mode. It runs from the
// main loop, once a pass.
func (a *App) idleTick(now time.Time) {
	a.mu.Lock()
	enabled, demo, going := a.IdleEnabled, a.Demo, a.farewell != ""
	idle, noPower := a.idle, a.noPower
	last := a.lastActivity
	prevSeen, prevOn := a.swSeen, a.swOn
	a.mu.Unlock()
	if going || a.State == nil {
		return
	}
	on, known, _ := a.State.Switch()
	if !enabled || demo { // the demo's made-up data has no switch to follow
		known = false
	}
	a.mu.Lock()
	edge := known && (!prevSeen || on != prevOn)
	a.swSeen, a.swOn = known, on
	if edge && !on {
		a.idleHold = time.Time{} // it went off just now: not the wake by hand's quiet
	}
	hold := a.idleHold
	a.mu.Unlock()

	want := known && !on
	switch {
	case want && !idle && !noPower && now.After(hold) && now.Sub(last) >= noPowerIdle:
		a.enterIdle(now)
	case !want && idle:
		a.exitIdle(now, false)
	}
}

// enterIdle goes to the IDLE screen and cuts the connection down.
func (a *App) enterIdle(now time.Time) {
	a.mu.Lock()
	if a.idle {
		a.mu.Unlock()
		return
	}
	a.idle = true
	a.sleepDrawn = ""
	before := a.lightLevel
	if before > 0 { // no-power mode may have turned it off already, and noted where it was
		a.lightBefore = before
	}
	path := a.idlePathLocked()
	a.mu.Unlock()
	log.Printf("idle: %s is off: idle mode (the screen stops updating)", path)

	if a.Light != nil && before > 0 {
		if err := a.Light.Set(0); err != nil {
			log.Printf("front light: %v", err)
		} else {
			a.mu.Lock()
			a.lightLevel = 0
			a.mu.Unlock()
		}
	}
	if a.OnIdle != nil {
		a.OnIdle(true, path)
	}
	a.nudge()
}

// exitIdle comes back: the light, the connection and the screen. manual says
// someone woke it by hand, which holds idle mode off for a while.
func (a *App) exitIdle(now time.Time, manual bool) {
	a.mu.Lock()
	if !a.idle {
		a.mu.Unlock()
		return
	}
	a.idle = false
	a.sleepDrawn = ""
	a.pageChanged = true // a whole new picture
	if manual {
		a.idleHold = now.Add(idleRewake)
	}
	before := 0
	if !a.noPower { // no-power mode, still on, keeps the light off and the note of where it was
		before = a.lightBefore
		a.lightBefore = 0
	}
	a.mu.Unlock()
	log.Print("idle: back from idle mode")

	if a.Light != nil && before > 0 {
		if err := a.Light.Set(before); err != nil {
			log.Printf("front light: %v", err)
		} else {
			a.mu.Lock()
			a.lightLevel = before
			a.mu.Unlock()
		}
	}
	if a.OnIdle != nil {
		a.OnIdle(false, "")
	}
	a.nudge()
}

// PreviewIdle puts the app in idle mode with none of the side effects, for drawing
// the IDLE screen on a PC.
func (a *App) PreviewIdle() {
	a.mu.Lock()
	a.idle = true
	a.mu.Unlock()
}

// SetIdleSwitch turns idle mode on or off and chooses the SignalK path of its
// switch ("" for settings.DefaultIdlePath). It is saved.
func (a *App) SetIdleSwitch(enabled bool, path string) error {
	if path != "" {
		if _, ok := signalk.MetaPath(path); !ok {
			return fmt.Errorf("%q is not a SignalK path like electrical.switches.kindle.state", path)
		}
	}
	a.activity()
	a.mu.Lock()
	a.IdleEnabled = enabled
	a.IdlePath = path
	if path == settings.DefaultIdlePath {
		a.IdlePath = "" // the default is not a choice
	}
	f := a.settingsFileLocked()
	a.pageChanged = true
	a.mu.Unlock()
	a.saveSettings(f)
	a.idleSettingChanged()
	return nil
}
