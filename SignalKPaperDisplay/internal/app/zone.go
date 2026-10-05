package app

import (
	"log"
	"strings"
	"time"

	"signalkpaperdisplay/internal/settings"
)

// The time zone the clock is shown in. A Kindle's own zone is whatever the stock
// software left it, usually UTC, so the dashboard keeps its own choice (an IANA
// name, from a database built into the program) and converts every time it draws.

// ApplyTimezone sets the zone at start-up, from the saved settings, and does not
// save it. An unusable name is logged and the device's own zone is used.
func (a *App) ApplyTimezone(name string) {
	a.mu.Lock()
	err := a.setTimezoneLocked(name)
	a.mu.Unlock()
	if err != nil {
		log.Printf("settings: time zone: %v (using the device's own)", err)
	}
}

// setTimezoneLocked changes the zone. The caller holds a.mu. A name that cannot be
// used changes nothing.
func (a *App) setTimezoneLocked(name string) error {
	loc, err := settings.ParseTimezone(name)
	if err != nil {
		return err
	}
	a.zone = loc
	a.Timezone = strings.TrimSpace(name)
	if a.Timezone == "Local" {
		a.Timezone = ""
	}
	return nil
}

// inZoneLocked is t in the chosen zone. The caller holds a.mu.
func (a *App) inZoneLocked(t time.Time) time.Time {
	if a.zone != nil {
		return t.In(a.zone)
	}
	return t
}

// clockLocked is the time of day in the chosen zone, "14:32". The caller holds a.mu.
func (a *App) clockLocked(t time.Time) string { return a.inZoneLocked(t).Format("15:04") }

// SetTimezone chooses the zone ("" for the device's own), and saves it. Every
// clock on screen changes, so it is a full refresh.
func (a *App) SetTimezone(name string) error {
	a.activity()
	a.mu.Lock()
	if err := a.setTimezoneLocked(name); err != nil {
		a.mu.Unlock()
		return err
	}
	zone := a.Timezone
	a.pageChanged = true
	f := a.settingsFileLocked()
	a.mu.Unlock()
	log.Printf("remote: time zone is now %s", map[bool]string{true: zone, false: "the device's own"}[zone != ""])
	a.saveSettings(f)
	a.nudge()
	return nil
}
