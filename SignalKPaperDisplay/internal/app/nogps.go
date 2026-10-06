package app

import (
	"fmt"
	"log"
	"time"

	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
)

// The NO DATA banner has one rule that depends on a setting: our own GPS position
// not having been updated for a while (see pages.Lost). A GPS fix comes about once a
// second, so ten seconds is ten or so missed fixes; the setting is how many seconds,
// and zero turns the rule off. The other rules - the server link down, or nothing at
// all arriving - always apply.

var errBadNoGPS = fmt.Errorf("the GPS timeout is 0 (off) or %d to %d seconds", settings.MinNoGPSSeconds, settings.MaxNoGPSSeconds)

// gpsTimeoutLocked is the GPS timeout as a duration; zero means off. The caller holds
// a.mu.
func (a *App) gpsTimeoutLocked() time.Duration { return time.Duration(a.NoGPSSec) * time.Second }

func (a *App) gpsTimeout() time.Duration {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.gpsTimeoutLocked()
}

// SetNoGPSSeconds sets how long our own GPS position may go without an update
// before the header says NO DATA: from 2 seconds to an hour, or 0 for off. It is
// saved.
func (a *App) SetNoGPSSeconds(seconds int) error {
	if !settings.ValidNoGPS(seconds) {
		return errBadNoGPS
	}
	a.activity()
	a.mu.Lock()
	a.NoGPSSec = seconds
	a.NoGPSChosen = true
	f := a.settingsFileLocked()
	a.mu.Unlock()
	log.Printf("remote: NO DATA after %s without a GPS position", pages.NoGPSLabel(seconds))
	a.saveSettings(f)
	a.nudge()
	return nil
}
