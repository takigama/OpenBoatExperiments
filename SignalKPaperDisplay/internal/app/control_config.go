package app

import (
	"fmt"
	"log"
	"sort"
	"strings"

	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/units"
)

// The configuration changes of the web page: demo mode, the SignalK server and
// the units. Like the display controls beside them they do what the touch
// screen does - the same validation, the same saving, the same redraw.

// SetDemoMode switches demo mode on or off, as the settings switch does. It is
// never saved: the app starts on the real server whatever it was left at.
func (a *App) SetDemoMode(on bool) {
	a.activity()
	a.mu.Lock()
	changed := a.Demo != on
	a.Demo = on
	a.pageChanged = true
	a.mu.Unlock()
	if !changed {
		a.nudge()
		return
	}
	log.Printf("remote: demo mode is now %s", map[bool]string{true: "on", false: "off"}[on])
	if a.OnDemoChange != nil {
		a.OnDemoChange(on)
	}
	a.nudge()
}

// SetServer points the dashboard at a different SignalK server, "host:port" (the
// port defaults to 3000), and saves it. An empty address goes back to the
// default - the -signalk flag, which is the launcher's SIGNALK_HOST. The
// connection moves at once and what the old server sent is forgotten.
func (a *App) SetServer(hostPort string) error {
	a.activity()
	norm := ""
	if strings.TrimSpace(hostPort) != "" {
		n, err := settings.NormalizeServer(hostPort)
		if err != nil {
			return fmt.Errorf("%q: %w", hostPort, err)
		}
		norm = n
	}
	a.mu.Lock()
	before := a.serverLocked()
	if norm == a.DefaultServer {
		norm = "" // the default is not a choice: the launcher's setting keeps applying
	}
	a.Server = norm
	after := a.serverLocked()
	f := a.settingsFileLocked()
	if before != after {
		a.metaTried = nil // a different server has to be asked about units afresh
		a.pageChanged = true
	}
	a.mu.Unlock()
	a.saveSettings(f)
	if before == after {
		return nil
	}
	log.Printf("remote: SignalK server is now %s", after)
	if a.OnServerChange != nil {
		a.OnServerChange(after)
	}
	a.syncWatch()
	a.nudge()
	return nil
}

// SetUnits changes the units: the preset (which resets every unit to it), then
// individual overrides on top of it, a metric ID to a unit symbol. It is all or
// nothing, and saved.
func (a *App) SetUnits(preset string, overrides map[string]string) error {
	a.activity()
	u := a.unitsNow() // a copy to change and check, so a bad request changes nothing
	if preset != "" {
		if err := u.SetPreset(preset); err != nil {
			return err
		}
	}
	ids := make([]string, 0, len(overrides))
	for id := range overrides {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		if err := u.SetUnit(id, overrides[id]); err != nil {
			return err
		}
	}
	a.mu.Lock()
	a.Units = u
	a.pageChanged = true // every number changes: a full refresh, as from the touch screen
	f := a.settingsFileLocked()
	a.mu.Unlock()
	a.saveSettings(f)
	log.Printf("remote: units are now %s (%d overridden)", u.Preset, len(u.Overrides))
	a.nudge()
	return nil
}

var errBadNoPower = fmt.Errorf("no-power mode is 0 (never) or 1 to %d minutes", settings.MaxNoPowerMinutes)

// noPowerWords says a timeout in words, for the log.
func noPowerWords(minutes int) string { return pages.NoPowerLabel(minutes) }

// UnitsNow is the current unit settings, a copy.
func (a *App) UnitsNow() units.Settings { return a.unitsNow() }
