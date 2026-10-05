package app

import (
	"testing"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/input"
)

func TestHandleEventSwitchesPages(t *testing.T) {
	// Three pages, in the order compass, nav, map: start on the last, so "next"
	// has to wrap around.
	a := &App{Display: &display.PNG{W: 900, H: 1200}}
	if !a.SetPage("map") {
		t.Fatal("map page missing")
	}
	a.takePageChanged()

	check := func(step string, ev input.Event, wantID string, wantChange bool) {
		t.Helper()
		a.HandleEvent(ev)
		if got := a.currentPage().ID; got != wantID {
			t.Errorf("%s: page = %s, want %s", step, got, wantID)
		}
		if got := a.takePageChanged(); got != wantChange {
			t.Errorf("%s: pageChanged = %v, want %v", step, got, wantChange)
		}
	}

	check("tap right third wraps to the first", input.Event{Kind: input.Tap, X: 850, Y: 600}, "compass", true)
	check("tap middle is inert", input.Event{Kind: input.Tap, X: 450, Y: 600}, "compass", false)
	check("tap left third wraps back", input.Event{Kind: input.Tap, X: 50, Y: 600}, "map", true)
	check("swipe left = next", input.Event{Kind: input.SwipeLeft, X: 800, Y: 600}, "compass", true)
	check("swipe right = previous", input.Event{Kind: input.SwipeRight, X: 100, Y: 600}, "map", true)
	check("previous", input.Event{Kind: input.Tap, X: 50, Y: 600}, "nav", true)
	check("long press ignored", input.Event{Kind: input.LongPress, X: 850, Y: 600}, "nav", false)
}
