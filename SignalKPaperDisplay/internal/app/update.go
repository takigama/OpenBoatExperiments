package app

import (
	"errors"
	"fmt"
	"log"
	"time"
)

// Update now. The app already checks for a newer release every few hours and
// restarts into it; this is the same thing on demand, from the settings screen or
// the web page, with the answer shown where it was asked.

// UpdateResult is what an update check found.
type UpdateResult struct {
	Current   int  // the version running
	Newest    int  // the newest the manifest lists
	Installed bool // a newer one was downloaded and put in place
}

// updateRestartDelay is how long after an install the screen is left saying so,
// before the app is restarted into the new version: long enough for the e-ink
// panel to finish drawing the message.
var updateRestartDelay = 4 * time.Second

// UpdateNow looks for a newer release in the background and installs it if there
// is one; the answer is in Control (UpdateMsg). It does nothing if a check is
// already running. After an install the app is restarted (OnRestart).
func (a *App) UpdateNow() error {
	if a.OnUpdate == nil {
		return errors.New("this display cannot update itself")
	}
	a.activity()
	a.mu.Lock()
	if a.updateBusy {
		a.mu.Unlock()
		return nil
	}
	a.updateBusy, a.updateNew = true, false
	a.updateMsg = "Looking for a newer release..."
	a.mu.Unlock()
	log.Print("update: asked for from the screen or the web page")
	a.nudge()
	go a.runUpdateNow()
	return nil
}

func (a *App) runUpdateNow() {
	res, err := a.OnUpdate()
	a.mu.Lock()
	a.updateBusy = false
	switch {
	case err != nil:
		a.updateMsg = "Update failed: " + err.Error()
	case res.Installed:
		a.updateMsg = fmt.Sprintf("Installed v%d: restarting...", res.Newest)
	case res.Newest > res.Current: // found, and not installed: cannot happen, but say so
		a.updateMsg, a.updateNew = fmt.Sprintf("v%d is available", res.Newest), true
	default:
		a.updateMsg = fmt.Sprintf("Up to date: v%d is the newest", res.Current)
	}
	msg := a.updateMsg
	a.force = true // the message is drawn in place
	a.mu.Unlock()
	log.Printf("update: %s", msg)
	a.nudge()
	if err == nil && res.Installed && a.OnRestart != nil {
		time.Sleep(updateRestartDelay)
		a.OnRestart()
	}
}
