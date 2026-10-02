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
	Units            units.Settings

	mu          sync.Mutex // guards page state, which touch input will change
	page        int
	pageChanged bool
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

func (a *App) Show(now time.Time, full bool) error {
	img, err := a.Frame(now)
	if err != nil {
		return err
	}
	return a.Display.Show(img, full)
}

// Run redraws every Interval until ctx is cancelled.
func (a *App) Run(ctx context.Context) {
	t := time.NewTicker(a.Interval)
	defer t.Stop()
	var lastFull time.Time // zero, so the first frame is a full refresh
	for {
		now := time.Now()
		full := a.takePageChanged() || lastFull.IsZero() ||
			(a.FullRefreshEvery > 0 && now.Sub(lastFull) >= a.FullRefreshEvery)
		if err := a.Show(now, full); err != nil {
			log.Printf("display: %v", err)
		} else if full {
			lastFull = now
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}
