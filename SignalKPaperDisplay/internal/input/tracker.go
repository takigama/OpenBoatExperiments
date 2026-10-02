// Package input turns raw touchscreen events into simple gestures (tap, long
// press, swipe). The Tracker here is pure logic with no device access, so
// it's tested with synthetic events; evdev_linux.go feeds it from a real
// /dev/input node.
package input

import (
	"math"
	"time"
)

// Linux input event codes for type-B multitouch (see linux/input-event-codes.h).
const (
	evSyn = 0x00
	evKey = 0x01
	evAbs = 0x03

	absMTSlot       = 0x2f
	absMTPositionX  = 0x35
	absMTPositionY  = 0x36
	absMTTrackingID = 0x39

	btnTouch = 0x14a
)

type Kind int

const (
	Tap Kind = iota
	LongPress
	SwipeLeft
	SwipeRight
	SwipeUp
	SwipeDown
)

func (k Kind) String() string {
	return [...]string{"tap", "long-press", "swipe-left", "swipe-right", "swipe-up", "swipe-down"}[k]
}

// Event is a finished gesture. X, Y are screen pixels where the finger
// first touched.
type Event struct {
	Kind Kind
	X, Y int
}

// Raw is one kernel input event.
type Raw struct {
	Type, Code uint16
	Value      int32
	At         time.Time
}

type contact struct {
	down           time.Time
	rawX, rawY     int32 // latest raw position
	startX, startY int   // screen position at touch-down
	haveStart      bool
}

// Tracker recognises gestures from a stream of Raw events.
type Tracker struct {
	// Map converts raw device coordinates to screen pixels. Orientation
	// quirks (axes swapped or flipped) are handled there.
	Map func(rawX, rawY int32) (x, y int)

	TapMaxMove  float64       // pixels a finger may drift and still be a tap
	LongPress   time.Duration // held at least this long = long press
	SwipeMinLen float64       // pixels of travel that make a swipe

	slot     int
	contacts map[int]*contact
}

func NewTracker(m func(rawX, rawY int32) (x, y int)) *Tracker {
	return &Tracker{
		Map: m,
		// Measured on a Paperwhite 3: a normal press drifts 40-60px between
		// touch-down and lift, so the tap tolerance has to be generous.
		TapMaxMove:  80,
		LongPress:   700 * time.Millisecond,
		SwipeMinLen: 200,
		contacts:    map[int]*contact{},
	}
}

func (t *Tracker) cur() *contact {
	c := t.contacts[t.slot]
	if c == nil {
		c = &contact{}
		t.contacts[t.slot] = c
	}
	return c
}

// Feed consumes one raw event and returns a gesture when a finger lifts.
func (t *Tracker) Feed(r Raw) (Event, bool) {
	switch {
	case r.Type == evAbs && r.Code == absMTSlot:
		t.slot = int(r.Value)

	case r.Type == evAbs && r.Code == absMTTrackingID:
		if r.Value >= 0 {
			t.contacts[t.slot] = &contact{down: r.At}
			return Event{}, false
		}
		c := t.contacts[t.slot]
		delete(t.contacts, t.slot)
		if c == nil || !c.haveStart {
			return Event{}, false
		}
		return t.classify(c, r.At)

	case r.Type == evAbs && r.Code == absMTPositionX:
		c := t.cur()
		c.rawX = r.Value
		t.noteStart(c)

	case r.Type == evAbs && r.Code == absMTPositionY:
		c := t.cur()
		c.rawY = r.Value
		t.noteStart(c)
	}
	return Event{}, false
}

// noteStart records the first position of a contact. X and Y arrive as
// separate events, so wait until both have been seen before trusting it -
// otherwise a start of (x, 0) would look like a long drag.
func (t *Tracker) noteStart(c *contact) {
	if c.haveStart || c.rawX == 0 || c.rawY == 0 {
		return
	}
	c.startX, c.startY = t.Map(c.rawX, c.rawY)
	c.haveStart = true
}

func (t *Tracker) classify(c *contact, up time.Time) (Event, bool) {
	endX, endY := t.Map(c.rawX, c.rawY)
	dx, dy := float64(endX-c.startX), float64(endY-c.startY)
	dist := math.Hypot(dx, dy)
	held := up.Sub(c.down)

	switch {
	case dist >= t.SwipeMinLen:
		k := SwipeRight
		switch {
		case math.Abs(dx) >= math.Abs(dy) && dx < 0:
			k = SwipeLeft
		case math.Abs(dx) < math.Abs(dy) && dy < 0:
			k = SwipeUp
		case math.Abs(dx) < math.Abs(dy):
			k = SwipeDown
		}
		return Event{k, c.startX, c.startY}, true
	case dist <= t.TapMaxMove && held >= t.LongPress:
		return Event{LongPress, c.startX, c.startY}, true
	case dist <= t.TapMaxMove:
		return Event{Tap, c.startX, c.startY}, true
	}
	return Event{}, false // drifted too far to be a tap, too short to be a swipe
}
