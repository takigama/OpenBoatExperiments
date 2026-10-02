// Package app ties the pieces together: it renders the current page from the
// live SignalK state and pushes frames to whatever Display it was given.
package app

import (
	"context"
	"image"
	"log"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
)

type App struct {
	State    *signalk.State
	Display  display.Display
	Interval time.Duration
	// FullRefreshEvery forces a flashing full-panel refresh this often, to
	// clear the ghosting that accumulates from partial updates. Zero means
	// only the first frame is a full refresh.
	FullRefreshEvery time.Duration
}

// Frame renders the current page for the given moment.
func (a *App) Frame(now time.Time) (*image.Gray, error) {
	w, h := a.Display.Size()
	c, err := render.NewCanvas(w, h)
	if err != nil {
		return nil, err
	}
	pages.Nav(c, a.State.Snapshot(), now)
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
		full := lastFull.IsZero() || (a.FullRefreshEvery > 0 && now.Sub(lastFull) >= a.FullRefreshEvery)
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
