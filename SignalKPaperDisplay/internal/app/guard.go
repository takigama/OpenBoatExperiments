package app

import (
	"errors"
	"image"
	"log"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/render"
)

// guardHeader repaints the header strip now and then, whatever the app thinks
// is on the screen.
//
// Something else on the device - the stock UI's status bar, which keeps
// running after the Kindle framework is stopped - draws into the same panel,
// the clock above all, at the start of each minute. We skip frames identical
// to the last one sent, so once it has painted over our header nothing here
// changes and the stock clock would stay until our own clock next ticks over.
// Repainting the header regardless puts ours back within a few seconds, and
// it costs almost nothing: under the fast waveform, pixels that are already
// right don't flicker.
//
// It runs from the main loop, shortly after each minute begins (when the
// stock clock is drawn) and every HeaderGuardEvery besides. It does nothing on
// displays that can't update a region.
func (a *App) guardHeader(now time.Time) {
	if a.HeaderGuardEvery <= 0 {
		return
	}
	rd, ok := a.Display.(display.RegionDisplay)
	if !ok {
		return
	}
	minute := now.Unix() / 60
	due := now.Sub(a.lastGuard) >= a.HeaderGuardEvery ||
		(minute != a.guardMinute && now.Second() >= 2) // after the stock clock's own repaint
	if !due {
		return
	}
	a.lastGuard, a.guardMinute = now, minute

	img, err := a.Frame(now) // inversion, if on, is already applied
	if err != nil {
		log.Printf("header guard: %v", err)
		return
	}
	band := render.ScaleRect(pages.HeaderBand(pages.DesignWidth), a.designScale())
	err = rd.ShowRegion(img.SubImage(band).(*image.Gray), band.Min.X, band.Min.Y)
	if err != nil && !errors.Is(err, display.ErrNoRegion) && err.Error() != a.lastGuardErr {
		a.lastGuardErr = err.Error()
		log.Printf("header guard: %v", err)
	}
}
