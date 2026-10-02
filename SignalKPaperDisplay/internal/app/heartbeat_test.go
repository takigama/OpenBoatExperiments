package app

import (
	"errors"
	"image"
	"testing"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/signalk"
)

// regionRecorder is a Display that can also update a region, and remembers
// what it was asked to draw.
type regionRecorder struct {
	display.PNG
	regions []struct {
		x, y int
		img  *image.Gray
	}
	err error
}

func (r *regionRecorder) ShowRegion(img *image.Gray, x, y int) error {
	r.regions = append(r.regions, struct {
		x, y int
		img  *image.Gray
	}{x, y, img})
	return r.err
}

func litPixels(img *image.Gray) int {
	n := 0
	for y := img.Bounds().Min.Y; y < img.Bounds().Max.Y; y++ {
		for x := img.Bounds().Min.X; x < img.Bounds().Max.X; x++ {
			if img.GrayAt(x, y).Y > 200 {
				n++
			}
		}
	}
	return n
}

func TestHeartbeatBlinksTheDotInItsOwnRegion(t *testing.T) {
	rec := &regionRecorder{PNG: display.PNG{W: 1072, H: 1448}}
	a := &App{State: signalk.NewState(), Display: rec}
	rect := pages.HeartbeatRect(1072)

	// No data yet, so the banner is showing: the dot is white on black, and
	// "lit" means there are white pixels.
	a.heartbeat(time.Unix(2000, 0)) // even second: lit
	a.heartbeat(time.Unix(2001, 0)) // odd second: unlit
	if len(rec.regions) != 2 {
		t.Fatalf("got %d region updates, want 2", len(rec.regions))
	}
	for _, r := range rec.regions {
		if r.x != rect.Min.X || r.y != rect.Min.Y {
			t.Errorf("region drawn at (%d,%d), want the heartbeat rect's corner (%d,%d)", r.x, r.y, rect.Min.X, rect.Min.Y)
		}
	}
	if litPixels(rec.regions[0].img) == 0 || litPixels(rec.regions[1].img) != 0 {
		t.Error("the dot should be lit on the even second and dark on the odd one")
	}
}

func TestHeartbeatSkipsSettingsAndDisplaysWithoutRegions(t *testing.T) {
	rec := &regionRecorder{PNG: display.PNG{W: 1072, H: 1448}}
	a := &App{State: signalk.NewState(), Display: rec}
	a.settingsOpen = true
	a.heartbeat(time.Unix(2000, 0))
	if len(rec.regions) != 0 {
		t.Error("the settings screens have no header dot, so nothing should be drawn there")
	}

	// A display that can't do regions (the PNG preview, eips) just gets no heartbeat.
	plain := &App{State: signalk.NewState(), Display: &display.PNG{W: 1072, H: 1448}}
	plain.heartbeat(time.Unix(2000, 0)) // must not panic
}

func TestHeartbeatFailuresAreLoggedOnceNotEverySecond(t *testing.T) {
	rec := &regionRecorder{PNG: display.PNG{W: 1072, H: 1448}, err: errors.New("fbink exploded")}
	a := &App{State: signalk.NewState(), Display: rec}
	for i := 0; i < 5; i++ {
		a.heartbeat(time.Unix(2000+int64(i), 0))
	}
	if a.lastHeartbeatErr != "fbink exploded" {
		t.Errorf("lastHeartbeatErr = %q, want the error remembered", a.lastHeartbeatErr)
	}
	// An unsupported display is not an error worth reporting at all.
	rec2 := &regionRecorder{PNG: display.PNG{W: 1072, H: 1448}, err: display.ErrNoRegion}
	b := &App{State: signalk.NewState(), Display: rec2}
	b.heartbeat(time.Unix(2000, 0))
	if b.lastHeartbeatErr != "" {
		t.Errorf("ErrNoRegion should be silent, got %q", b.lastHeartbeatErr)
	}
}
