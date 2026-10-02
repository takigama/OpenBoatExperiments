// Package app ties the pieces together: it renders the current page from the
// live SignalK state and pushes frames to whatever Display it was given.
package app

import (
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"log"
	"sync"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/input"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

type App struct {
	State    *signalk.State
	Display  display.Display
	Interval time.Duration
	// FullRefreshEvery forces a flashing full-panel refresh this often, to
	// clear the ghosting that accumulates from partial updates. Zero means
	// only the first frame is a full refresh.
	FullRefreshEvery time.Duration
	// MinRefresh is the shortest gap between partial refreshes; zero means
	// redraw whenever the picture changes.
	MinRefresh time.Duration
	// Units are the user's unit choices. They change from the touch handler
	// while the renderer reads them, so inside the app go through unitsNow()
	// and the settings handler, never touch the field directly once running.
	Units units.Settings
	// Invert shows white on black instead of black on white. Applied as the
	// last step of every frame, so it covers every page with no per-page work.
	Invert bool
	// Boxes is what each of the Nav page's six boxes shows (page box-kind IDs;
	// short or unknown entries take their defaults). Like Units, it changes
	// from the touch handler, so inside the app use boxesNow().
	Boxes []string
	// SettingsPath is where settings changes are saved; empty means don't persist.
	SettingsPath string

	mu           sync.Mutex // guards the fields below, which touch input changes
	page         int
	pageChanged  bool
	settingsOpen bool
	settingsView pages.SettingsView
	wake         chan struct{} // nudges Run to redraw now, not at the next tick

	renderTook       time.Duration // last frame's page rendering, only touched by Show
	lastHeartbeatErr string        // only touched by the main loop
}

func (a *App) nudge() {
	a.mu.Lock()
	if a.wake == nil {
		a.wake = make(chan struct{}, 1)
	}
	w := a.wake
	a.mu.Unlock()
	select {
	case w <- struct{}{}:
	default: // a redraw is already pending
	}
}

func (a *App) wakeChan() <-chan struct{} {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.wake == nil {
		a.wake = make(chan struct{}, 1)
	}
	return a.wake
}

// PrevPage cycles to the preceding page, wrapping around.
func (a *App) PrevPage() {
	n := len(pages.All())
	a.mu.Lock()
	a.page = (a.page + n - 1) % n
	a.pageChanged = true
	a.mu.Unlock()
}

// OpenSettings shows a settings screen directly, for previews.
func (a *App) OpenSettings(v pages.SettingsView) {
	a.mu.Lock()
	a.settingsOpen, a.settingsView, a.pageChanged = true, v, true
	a.mu.Unlock()
}

// unitsNow returns a private copy of the current unit settings.
func (a *App) unitsNow() units.Settings {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.Units.Clone()
}

// HandleEvent reacts to a touch gesture. On a normal page, tapping the cog
// opens settings, tapping the right third (or swiping left) goes to the next
// page, and the left third (or swiping right) to the previous one. The
// middle is deliberately inert, so a stray tap there does nothing. While
// settings are open every tap belongs to the settings screens.
func (a *App) HandleEvent(ev input.Event) {
	a.mu.Lock()
	open := a.settingsOpen
	a.mu.Unlock()
	if open {
		if ev.Kind == input.Tap {
			a.handleSettingsTap(ev)
		}
		return
	}
	if ev.Kind == input.Tap && image.Pt(ev.X, ev.Y).In(pages.CogRect) {
		a.mu.Lock()
		a.settingsOpen, a.settingsView, a.pageChanged = true, pages.SettingsView{}, true
		a.mu.Unlock()
		log.Printf("touch: tap at (%d,%d) -> settings", ev.X, ev.Y)
		a.nudge()
		return
	}

	w, _ := a.Display.Size()
	action := ""
	switch {
	case ev.Kind == input.SwipeLeft, ev.Kind == input.Tap && ev.X > w*2/3:
		a.NextPage()
		action = "next page"
	case ev.Kind == input.SwipeRight, ev.Kind == input.Tap && ev.X < w/3:
		a.PrevPage()
		action = "previous page"
	}
	// Logged either way: on an e-ink screen the redraw lags, so the log is
	// the only immediate proof that a touch was recognised at all.
	if action == "" {
		log.Printf("touch: %s at (%d,%d) - no action there", ev.Kind, ev.X, ev.Y)
		return
	}
	log.Printf("touch: %s at (%d,%d) -> %s (%s)", ev.Kind, ev.X, ev.Y, action, a.currentPage().ID)
	a.nudge()
}

// SetPage switches to the page with the given ID, reporting whether it exists.
func (a *App) SetPage(id string) bool {
	for i, p := range pages.All() {
		if p.ID == id {
			a.mu.Lock()
			a.page, a.pageChanged = i, true
			a.mu.Unlock()
			return true
		}
	}
	return false
}

// NextPage cycles to the following page, wrapping around.
func (a *App) NextPage() {
	a.mu.Lock()
	a.page = (a.page + 1) % len(pages.All())
	a.pageChanged = true
	a.mu.Unlock()
}

func (a *App) currentPage() pages.Page {
	a.mu.Lock()
	defer a.mu.Unlock()
	return pages.All()[a.page]
}

// takePageChanged reports (and clears) whether the page switched since the
// last frame. A different page is a completely different picture, so it's
// shown with a full refresh rather than ghosting over the old one.
func (a *App) takePageChanged() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	changed := a.pageChanged
	a.pageChanged = false
	return changed
}

// handleSettingsTap applies one tap on a settings screen.
func (a *App) handleSettingsTap(ev input.Event) {
	a.mu.Lock()
	view := a.settingsView
	a.mu.Unlock()

	w, _ := a.Display.Size()
	act := pages.SettingsTap(view, ev.X, ev.Y, w)
	if act.Kind == pages.ActNone {
		return
	}

	a.mu.Lock()
	var err error
	switch act.Kind {
	case pages.ActBack:
		if view.Screen == pages.SettingsRoot {
			a.settingsOpen = false
		} else {
			a.settingsView = pages.SettingsView{Screen: pages.ParentScreen(view.Screen)}
		}
	case pages.ActOpenPresetPicker:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPickPreset}
	case pages.ActOpenUnitPicker:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPickUnit, Metric: act.Metric}
	case pages.ActSetPreset:
		err = a.Units.SetPreset(act.Value)
		a.settingsView = pages.SettingsView{}
	case pages.ActSetUnit:
		err = a.Units.SetUnit(act.Metric, act.Value)
		a.settingsView = pages.SettingsView{}
	case pages.ActToggleInvert:
		a.Invert = !a.Invert // stays on the list: you see the result at once
	case pages.ActOpenBoxes:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsBoxes}
	case pages.ActOpenBoxPicker:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPickBox, Box: act.Box}
	case pages.ActSetBox:
		a.Boxes = pages.NormalizeBoxes(a.Boxes)
		a.Boxes[act.Box] = act.Value
		a.settingsView = pages.SettingsView{Screen: pages.SettingsBoxes}
	}
	changed := act.Kind == pages.ActSetPreset || act.Kind == pages.ActSetUnit ||
		act.Kind == pages.ActToggleInvert || act.Kind == pages.ActSetBox
	saved := settings.File{Settings: a.Units.Clone(), Invert: a.Invert, Boxes: append([]string(nil), a.Boxes...)}
	a.pageChanged = true // every screen is a different picture: full refresh
	a.mu.Unlock()

	if err != nil {
		log.Printf("settings: %v", err)
	}
	if changed && a.SettingsPath != "" {
		if err := settings.Save(a.SettingsPath, saved); err != nil {
			log.Printf("settings: could not save %s: %v", a.SettingsPath, err)
		}
	}
	log.Printf("touch: settings tap at (%d,%d)", ev.X, ev.Y)
	a.nudge()
}

// heartbeat blinks the dot beside the clock, once per loop, by redrawing just
// its small rectangle. It runs from the main loop on purpose: if the app
// hangs, the dot stops, which is the point of having it. It does nothing on
// displays that can't update a region, and on the settings screens, which
// have no header clock to sit beside.
func (a *App) heartbeat(now time.Time) {
	rd, ok := a.Display.(display.RegionDisplay)
	if !ok {
		return
	}
	a.mu.Lock()
	open := a.settingsOpen
	a.mu.Unlock()
	if open {
		return
	}
	w, _ := a.Display.Size()
	img, rect := pages.HeartbeatImage(w, pages.HeartbeatOn(now), pages.Lost(a.State.Snapshot(), now))
	if img == nil {
		return
	}
	if a.invertNow() {
		img = invertedCopy(img)
	}
	err := rd.ShowRegion(img, rect.Min.X, rect.Min.Y)
	// Once a second would flood the log if it fails, so say it once per distinct error.
	if err != nil && !errors.Is(err, display.ErrNoRegion) && err.Error() != a.lastHeartbeatErr {
		a.lastHeartbeatErr = err.Error()
		log.Printf("heartbeat: %v", err)
	}
}

// Frame renders the current page (or the settings screens) for the given moment.
func (a *App) Frame(now time.Time) (*image.Gray, error) {
	w, h := a.Display.Size()
	c, err := render.NewCanvas(w, h)
	if err != nil {
		return nil, err
	}
	a.mu.Lock()
	open, view := a.settingsOpen, a.settingsView
	a.mu.Unlock()
	u := a.unitsNow()
	invert := a.invertNow()
	boxes := a.boxesNow()
	if open {
		pages.Settings(c, view, u, invert, boxes)
	} else {
		a.currentPage().Draw(c, a.State.Snapshot(), now, pages.Env{Units: u, Boxes: boxes})
	}
	if invert {
		invertInPlace(c.Img)
	}
	return c.Img, nil
}

// boxesNow returns a private, normalised copy of the Nav box layout.
func (a *App) boxesNow() []string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return pages.NormalizeBoxes(a.Boxes)
}

func (a *App) invertNow() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.Invert
}

// invertInPlace flips every pixel: black becomes white and the reverse, with
// greys mirrored. Done once at the end of a frame rather than by every page.
func invertInPlace(img *image.Gray) {
	for i, v := range img.Pix {
		img.Pix[i] = 255 - v
	}
}

// invertedCopy is invertInPlace on a copy, for images that share pixels with
// another (the heartbeat's region is a window into a larger canvas).
func invertedCopy(src *image.Gray) *image.Gray {
	b := src.Bounds()
	dst := image.NewGray(b)
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			dst.SetGray(x, y, color.Gray{Y: 255 - src.GrayAt(x, y).Y})
		}
	}
	return dst
}

func (a *App) Show(now time.Time, full bool) (drew bool, err error) {
	t0 := time.Now()
	img, err := a.Frame(now)
	if err != nil {
		return false, err
	}
	a.renderTook = time.Since(t0)
	return a.Display.Show(img, full)
}

// timing describes where the last frame's time went: our own page
// rendering, plus the display's breakdown if it offers one.
func (a *App) timing() string {
	s := fmt.Sprintf("render %s", a.renderTook.Round(time.Millisecond))
	if t, ok := a.Display.(interface{ LastTiming() string }); ok {
		s += ", " + t.LastTiming()
	}
	return s
}

// Run redraws every Interval until ctx is cancelled.
func (a *App) Run(ctx context.Context) {
	t := time.NewTicker(a.Interval)
	defer t.Stop()
	var lastFull, lastDraw time.Time // zero, so the first frame is a full refresh
	for {
		now := time.Now()
		full := a.takePageChanged() || lastFull.IsZero() ||
			(a.FullRefreshEvery > 0 && now.Sub(lastFull) >= a.FullRefreshEvery)
		// Partial refreshes are rationed: a slow panel (eips takes ~3.5s a
		// frame) redrawing on every tick would be busy nearly all the time,
		// so a tap's page change would queue behind it. Full refreshes
		// (page changes, the periodic flash) always go straight through.
		if full || a.MinRefresh <= 0 || now.Sub(lastDraw) >= a.MinRefresh {
			started := time.Now()
			drew, err := a.Show(now, full)
			took := time.Since(started)
			if drew {
				lastDraw = started
			}
			switch {
			case err != nil:
				log.Printf("display: %v", err)
			case full:
				lastFull = now
				log.Printf("display: full refresh (%s) took %s (%s)", a.currentPage().ID, took.Round(time.Millisecond), a.timing())
			case drew:
				log.Printf("display: partial refresh took %s (%s)", took.Round(time.Millisecond), a.timing())
			}
		}
		a.heartbeat(time.Now())
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		case <-a.wakeChan():
		}
	}
}
