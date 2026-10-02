package pages

import (
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// Env is everything a page needs besides live data: the user's settings.
type Env struct {
	Units units.Settings
	Boxes []string // what the Nav page's six boxes show; see NormalizeBoxes
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
