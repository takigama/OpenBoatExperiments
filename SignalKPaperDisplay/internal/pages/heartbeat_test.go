package pages

import (
	"testing"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
)

func TestHeartbeatBlinksEverySecond(t *testing.T) {
	t0 := time.Unix(1000, 0)
	for i := 0; i < 6; i++ {
		on := HeartbeatOn(t0.Add(time.Duration(i) * time.Second))
		if want := i%2 == 0; on != want {
			t.Errorf("second %d: on = %v, want %v", i, on, want)
		}
	}
	// Within a second it doesn't change.
	if HeartbeatOn(t0) != HeartbeatOn(t0.Add(900*time.Millisecond)) {
		t.Error("the dot must hold its state for the whole second")
	}
}

func headerAt(t *testing.T, s signalk.Snapshot, now time.Time) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	Header(c, "TEST", s, now, Env{})
	return c
}

func TestHeartbeatDotAppearsInItsRectAndNotOverTheClock(t *testing.T) {
	live := base()
	on := time.Unix(2000, 0) // even second: lit
	off := on.Add(time.Second)
	live.LastMessage, live.Connected = on, true

	// Same minute for both, so the clock text is identical and only the dot differs.
	rect := HeartbeatRect(1072)
	lit := inked(headerAt(t, withMessageAt(live, on), on), rect)
	dark := inked(headerAt(t, withMessageAt(live, off), off), rect)
	if lit == 0 {
		t.Fatal("the dot should be drawn when lit")
	}
	if dark != 0 {
		t.Errorf("with the dot off its area should be empty, found %d inked pixels - the dot is touching the clock or title", dark)
	}
}

func withMessageAt(s signalk.Snapshot, at time.Time) signalk.Snapshot {
	s.LastMessage = at
	s.Connected = true
	return s
}

func TestHeartbeatIsWhiteOnTheNoDataBanner(t *testing.T) {
	// A black dot on the black banner would be invisible, so it flips.
	lostSnap := signalk.Snapshot{} // never connected: banner showing
	on := time.Unix(2000, 0)
	rect := HeartbeatRect(1072)

	lit := headerAt(t, lostSnap, on)
	offC := headerAt(t, lostSnap, on.Add(time.Second))

	var litWhite, offWhite int
	for y := rect.Min.Y; y < rect.Max.Y; y++ {
		for x := rect.Min.X; x < rect.Max.X; x++ {
			if lit.Img.GrayAt(x, y).Y > 200 {
				litWhite++
			}
			if offC.Img.GrayAt(x, y).Y > 200 {
				offWhite++
			}
		}
	}
	if litWhite == 0 {
		t.Error("on the banner the lit dot should be white")
	}
	if offWhite != 0 {
		t.Errorf("on the banner the unlit state should be plain black, found %d white pixels", offWhite)
	}
}

func TestHeartbeatImageMatchesWhatTheHeaderDraws(t *testing.T) {
	// The region update and the full frame must agree, or the dot would
	// change appearance whenever a full frame is drawn.
	on := time.Unix(2000, 0)
	rect := HeartbeatRect(1072)
	for _, banner := range []bool{false, true} {
		snap := withMessageAt(base(), on)
		if banner {
			snap = signalk.Snapshot{}
		}
		full := headerAt(t, snap, on)
		img, r := HeartbeatImage(1072, true, banner)
		if r != rect || img.Bounds().Dx() != rect.Dx() || img.Bounds().Dy() != rect.Dy() {
			t.Fatalf("banner=%v: region image is %v at %v, want %v at %v", banner, img.Bounds(), r, rect.Size(), rect)
		}
		mismatch := 0
		for y := 0; y < rect.Dy(); y++ {
			for x := 0; x < rect.Dx(); x++ {
				if img.GrayAt(rect.Min.X+x, rect.Min.Y+y).Y != full.Img.GrayAt(rect.Min.X+x, rect.Min.Y+y).Y {
					mismatch++
				}
			}
		}
		if mismatch != 0 {
			t.Errorf("banner=%v: the region image differs from the full frame in %d pixels", banner, mismatch)
		}
	}
}
