package pages

import (
	"time"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// Env is everything a page needs besides live data: the user's settings.
type Env struct {
	Units units.Settings
	Boxes []string // what the Nav page's six boxes show; see NormalizeBoxes
	// Battery is the device's own battery, shown in the middle of the header;
	// nil when it can't be read, and then nothing is drawn.
	Battery *battery.Status
	// The two things the user can tap to change on the compass page. Neither is
	// saved: both start the same way on every boot (apparent wind, SOG).
	WindTrue bool        // the wind speed widget shows the true wind, not the apparent
	Speed    SpeedSource // which speed the speed box shows
}

// SpeedSource says which speed the compass page's speed box shows.
type SpeedSource int

const (
	SpeedSOG SpeedSource = iota // speed over ground - the default
	SpeedSTW                    // speed through the water
	SpeedVMG                    // velocity made good to the wind
)

// Next is the source a tap on the box moves on to, going round in a circle.
func (s SpeedSource) Next() SpeedSource { return (s + 1) % 3 }

// Label is the name the box shows for a source.
func (s SpeedSource) Label() string {
	switch s {
	case SpeedSTW:
		return "STW"
	case SpeedVMG:
		return "VMG"
	}
	return "SOG"
}

// Page is one selectable screen. Draw must be a pure function of its
// arguments - no I/O - so every page can be previewed as a PNG on a PC.
type Page struct {
	ID    string // stable name, used by the -page flag and saved settings
	Title string // what the user sees when picking a page
	Draw  func(c *render.Canvas, s signalk.Snapshot, now time.Time, e Env)
}

// All lists the screens in the order they're cycled through.
func All() []Page {
	return []Page{
		{ID: "compass", Title: "Compass", Draw: Compass}, // the default, so it comes first
		{ID: "nav", Title: "Numbers", Draw: Nav},
	}
}

func ByID(id string) (Page, bool) {
	for _, p := range All() {
		if p.ID == id {
			return p, true
		}
	}
	return Page{}, false
}
