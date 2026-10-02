// Package app ties the pieces together: it renders the current page from the
// live SignalK state and pushes frames to whatever Display it was given.
package app

import (
	"context"
	"image"
	"log"
	"sync"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/input"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/render"
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
	Units      units.Settings

	mu          sync.Mutex // guards page state, which touch input changes
	page        int
	pageChanged bool
	wake        chan struct{} // nudges Run to redraw now, not at the next tick
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

// HandleEvent reacts to a touch gesture on a w x h screen: tap the right
// third (or swipe left) for the next page, the left third (or swipe right)
// for the previous one. The middle is deliberately inert for now - it's
// where settings will go - so a stray tap there does nothing.
func (a *App) HandleEvent(ev input.Event) {
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

// Frame renders the current page for the given moment.
func (a *App) Frame(now time.Time) (*image.Gray, error) {
	w, h := a.Display.Size()
	c, err := render.NewCanvas(w, h)
	if err != nil {
		return nil, err
	}
	a.currentPage().Draw(c, a.State.Snapshot(), now, pages.Env{Units: a.Units})
	return c.Img, nil
}

func (a *App) Show(now time.Time, full bool) (drew bool, err error) {
	img, err := a.Frame(now)
	if err != nil {
		return false, err
	}
	return a.Display.Show(img, full)
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
				log.Printf("display: full refresh (%s) took %s", a.currentPage().ID, took.Round(time.Millisecond))
			case drew:
				log.Printf("display: partial refresh took %s", took.Round(time.Millisecond))
			}
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		case <-a.wakeChan():
		}
	}
}
