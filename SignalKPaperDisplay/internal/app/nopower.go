package app

import (
	"log"
	"time"

	"signalkpaperdisplay/internal/settings"
)

// No-power mode. A dashboard that is not plugged in is running on its battery, and
// there is little point in keeping a screen current that nobody will see before the
// battery is flat. After NoPowerMin minutes off external power (an hour unless
// changed; zero is never) the app draws one big NO POWER screen - an e-ink screen
// keeps its picture with no power - and stops everything that costs power: it
// stops redrawing, drops the SignalK connection (OnNoPower tells main to), and turns
// the front light off. Plugging in brings it back by itself; so does a tap, a press
// of the power button, or any command from the web page, and after one of those it
// waits a whole timeout again.

const (
	// noPowerIdle is how long a touch or command must have been quiet before the
	// screen may go to NO POWER, so it never blanks in the middle of someone using it.
	noPowerIdle = 30 * time.Second
	// noPowerPoll is how often, in NO POWER, the battery is looked at to see if
	// power is back. Waking up more often would only cost what the mode saves.
	noPowerPoll = 30 * time.Second
)

// inNoPower reports whether the display is showing NO POWER.
func (a *App) inNoPower() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.noPower
}

// powerTick looks at the battery and moves in or out of no-power mode. It runs
// from the main loop, once a pass, which is what lets it read the battery.
func (a *App) powerTick(now time.Time) {
	a.mu.Lock()
	timeout := time.Duration(a.NoPowerMin) * time.Minute
	going := a.farewell != "" // being switched off: nothing to add
	active := a.noPower
	idle := now.Sub(a.lastActivity)
	a.mu.Unlock()
	if going {
		return
	}

	st := a.batteryNow(now)
	if st == nil || st.Plugged { // on power, or no way to tell: never conserve what is not known to be short
		a.mu.Lock()
		a.unpluggedSince = time.Time{}
		a.mu.Unlock()
		if active {
			a.exitNoPower(now, false)
		}
		return
	}

	a.mu.Lock()
	if a.unpluggedSince.IsZero() {
		a.unpluggedSince = now
		if timeout > 0 {
			log.Printf("power: running on the battery; no-power mode in %s", timeout)
		}
	}
	since := a.unpluggedSince
	a.mu.Unlock()

	switch {
	case timeout <= 0:
		if active { // changed to "never" while it was showing
			a.exitNoPower(now, false)
		}
	case !active && now.Sub(since) >= timeout && idle >= noPowerIdle:
		a.enterNoPower(now)
	}
}

// enterNoPower goes to the NO POWER screen and stops what costs power.
func (a *App) enterNoPower(now time.Time) {
	a.mu.Lock()
	if a.noPower {
		a.mu.Unlock()
		return
	}
	a.noPower = true
	a.sleepDrawn = ""
	before := a.lightLevel
	if before > 0 { // idle mode may have turned it off already, and noted where it was
		a.lightBefore = before
	}
	a.mu.Unlock()
	log.Printf("power: off external power for %d min: no-power mode (the screen stops updating)", a.NoPowerMin)

	if a.Light != nil && before > 0 { // the light is the biggest drain the app controls
		if err := a.Light.Set(0); err != nil {
			log.Printf("front light: %v", err)
		} else {
			a.mu.Lock()
			a.lightLevel = 0
			a.mu.Unlock()
		}
	}
	if a.OnNoPower != nil {
		a.OnNoPower(true)
	}
	a.nudge()
}

// exitNoPower comes back: the light, the connection and the screen. restart makes
// the timeout start again from now (after a tap), rather than from when power was
// last lost.
func (a *App) exitNoPower(now time.Time, restart bool) {
	a.mu.Lock()
	if !a.noPower {
		a.mu.Unlock()
		return
	}
	a.noPower = false
	a.sleepDrawn = ""
	a.pageChanged = true // a whole new picture
	if restart {
		a.unpluggedSince = now
	}
	before := 0
	if !a.idle { // idle mode, still on, keeps the light off and the note of where it was
		before = a.lightBefore
		a.lightBefore = 0
	}
	a.mu.Unlock()
	log.Print("power: back from no-power mode")

	if a.Light != nil && before > 0 {
		if err := a.Light.Set(before); err != nil {
			log.Printf("front light: %v", err)
		} else {
			a.mu.Lock()
			a.lightLevel = before
			a.mu.Unlock()
		}
	}
	if a.OnNoPower != nil {
		a.OnNoPower(false)
	}
	a.nudge()
}

// activity records that someone is using the dashboard, which wakes it from no-power
// mode and from idle mode, and holds both off for a while. It reports whether it
// woke it from either.
func (a *App) activity() bool {
	now := time.Now()
	a.mu.Lock()
	a.lastActivity = now
	was := a.noPower
	wasIdle := a.idle
	a.mu.Unlock()
	if was {
		a.exitNoPower(now, true)
	}
	if wasIdle {
		a.exitIdle(now, true)
	}
	return was || wasIdle
}

// Wake brings the dashboard back from no-power mode, if it is in it, for another
// timeout.
func (a *App) Wake() { a.activity() }

// PreviewNoPower puts the app in no-power mode with none of the side effects, for
// drawing the NO POWER screen on a PC.
func (a *App) PreviewNoPower() {
	a.mu.Lock()
	a.noPower = true
	a.mu.Unlock()
}

// SetNoPowerMinutes sets how long off external power before no-power mode: from 1
// minute to a week, or 0 for never. It is saved.
func (a *App) SetNoPowerMinutes(minutes int) error {
	if !settings.ValidNoPower(minutes) {
		return errBadNoPower
	}
	a.activity()
	a.mu.Lock()
	a.NoPowerMin = minutes
	a.NoPowerChosen = true
	f := a.settingsFileLocked()
	a.mu.Unlock()
	log.Printf("remote: no-power mode after %s", noPowerWords(minutes))
	a.saveSettings(f)
	a.nudge()
	return nil
}
