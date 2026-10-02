package input

import (
	"testing"
	"time"
)

var t0 = time.Unix(1000, 0)

// finger synthesises one touch: down at (x0,y0), drag to (x1,y1), lift
// after hold. Coordinates are used raw; the test mapping is the identity.
func finger(tr *Tracker, x0, y0, x1, y1 int32, hold time.Duration) (Event, bool) {
	feed := func(typ, code uint16, v int32, at time.Duration) (Event, bool) {
		return tr.Feed(Raw{Type: typ, Code: code, Value: v, At: t0.Add(at)})
	}
	feed(evAbs, absMTSlot, 0, 0)
	feed(evAbs, absMTTrackingID, 7, 0)
	feed(evAbs, absMTPositionX, x0, 0)
	feed(evAbs, absMTPositionY, y0, 0)
	if x1 != x0 || y1 != y0 {
		feed(evAbs, absMTPositionX, x1, hold/2)
		feed(evAbs, absMTPositionY, y1, hold/2)
	}
	return feed(evAbs, absMTTrackingID, -1, hold)
}

func identity(x, y int32) (int, int) { return int(x), int(y) }

func TestGestures(t *testing.T) {
	cases := []struct {
		name           string
		x0, y0, x1, y1 int32
		hold           time.Duration
		want           Kind
		ok             bool
	}{
		{"tap", 500, 700, 500, 700, 80 * time.Millisecond, Tap, true},
		{"tap with a little finger wobble", 500, 700, 515, 710, 90 * time.Millisecond, Tap, true},
		{"long press", 500, 700, 500, 700, time.Second, LongPress, true},
		{"swipe left", 800, 700, 300, 720, 200 * time.Millisecond, SwipeLeft, true},
		{"swipe right", 200, 700, 800, 690, 200 * time.Millisecond, SwipeRight, true},
		{"swipe up", 500, 1200, 510, 400, 200 * time.Millisecond, SwipeUp, true},
		{"swipe down", 500, 400, 490, 1200, 200 * time.Millisecond, SwipeDown, true},
		{"a 60px drift is still a tap (real fingers roll)", 500, 700, 545, 740, 100 * time.Millisecond, Tap, true},
		{"drift too far for a tap, too short for a swipe", 500, 700, 620, 700, 100 * time.Millisecond, 0, false},
	}
	for _, c := range cases {
		tr := NewTracker(identity)
		ev, ok := finger(tr, c.x0, c.y0, c.x1, c.y1, c.hold)
		if ok != c.ok || (ok && ev.Kind != c.want) {
			t.Errorf("%s: got (%v, %v), want (%v, %v)", c.name, ev.Kind, ok, c.want, c.ok)
		}
		if ok && (ev.X != int(c.x0) || ev.Y != int(c.y0)) {
			t.Errorf("%s: event at (%d,%d), want where the finger went down (%d,%d)", c.name, ev.X, ev.Y, c.x0, c.y0)
		}
	}
}

func TestMapIsAppliedToPositions(t *testing.T) {
	// Raw device is 2x the screen size and Y is flipped.
	tr := NewTracker(func(x, y int32) (int, int) { return int(x) / 2, 1000 - int(y)/2 })
	ev, ok := finger(tr, 400, 600, 400, 600, 50*time.Millisecond)
	if !ok || ev.Kind != Tap || ev.X != 200 || ev.Y != 700 {
		t.Errorf("got %+v ok=%v, want tap at (200,700)", ev, ok)
	}
}

func TestLiftWithoutPositionIsIgnored(t *testing.T) {
	tr := NewTracker(identity)
	tr.Feed(Raw{Type: evAbs, Code: absMTTrackingID, Value: 3, At: t0})
	if _, ok := tr.Feed(Raw{Type: evAbs, Code: absMTTrackingID, Value: -1, At: t0.Add(50 * time.Millisecond)}); ok {
		t.Error("a contact that never reported a position must not produce a gesture")
	}
}
