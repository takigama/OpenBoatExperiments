package pages

import (
	"time"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// DesignWidth is the width, in design units, that every page is laid out in.
// A screen of another width draws the same layout scaled (see render.Canvas),
// so it needs no pages of its own; the design height follows from the screen's
// shape. 1072 is the Kindle Paperwhite 3's width, which the layout was made on.
const DesignWidth = 1072

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
	// Depth is what the compass page's bottom right widget shows: the ID of any
	// single-number kind or a raw SignalK path. Empty is the default, depth. Not
	// saved: it is depth on every boot. See DepthFromID.
	Depth string
	// The map page: its range in nautical miles (zero is the default, 5; see
	// MapRanges) and whether north is up instead of our heading. Neither is saved.
	MapRange   int
	MapNorthUp bool
	// Demo is whether the data is made up: the header says so, on every page, so
	// that it can never be taken for a real boat's.
	Demo bool
}

// SpeedSource says what the compass page's speed box shows: one of the three
// speeds a tap cycles through, or - set from the web page - any single-number
// kind a Nav box can show, or a raw SignalK path. It is the kind's ID; the zero
// value is the default, speed over ground.
type SpeedSource string

const (
	SpeedSOG SpeedSource = ""     // speed over ground - the default
	SpeedSTW SpeedSource = "stw"  // speed through the water
	SpeedVMG SpeedSource = "vmgw" // velocity made good to the wind
)

// ID is the source's kind ID.
func (s SpeedSource) ID() string {
	if s == SpeedSOG {
		return "sog"
	}
	return string(s)
}

// SpeedFromID is the source for a kind ID, or false if the box cannot show it:
// it must be a kind with a single number (so not the closest-AIS box) or a path.
func SpeedFromID(id string) (SpeedSource, bool) {
	if id == "sog" || id == "" {
		return SpeedSOG, true
	}
	if id == BoxAIS {
		return SpeedSOG, false
	}
	if _, ok := BoxKindByID(id); !ok {
		return SpeedSOG, false
	}
	return SpeedSource(id), true
}

// Next is the source a tap on the box moves on to, going round in a circle of
// the three speeds. Anything else, chosen on the web page, goes back to SOG.
func (s SpeedSource) Next() SpeedSource {
	switch s {
	case SpeedSOG:
		return SpeedSTW
	case SpeedSTW:
		return SpeedVMG
	}
	return SpeedSOG
}

// Label is the name the box shows for a source.
func (s SpeedSource) Label() string {
	switch s {
	case SpeedSOG:
		return "SOG"
	case SpeedSTW:
		return "STW"
	case SpeedVMG:
		return "VMG"
	}
	if k, ok := BoxKindByID(string(s)); ok {
		return k.Label
	}
	return "SPEED"
}

// DepthFromID is the compass depth widget's setting for a kind ID, or false if the
// widget cannot show it: the same kinds as the speed widget. Depth, the default,
// is the empty string.
func DepthFromID(id string) (string, bool) {
	if id == "" || id == "depth" {
		return "", true
	}
	if _, ok := SpeedFromID(id); !ok {
		return "", false
	}
	return id, true
}

// DepthWidgetID is the kind ID the widget shows, "depth" for the default.
func DepthWidgetID(setting string) string {
	if setting == "" {
		return "depth"
	}
	return setting
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
		{ID: "map", Title: "Map", Draw: Map},
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
