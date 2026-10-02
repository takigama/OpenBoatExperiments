package pages

import (
	"image"
	"strings"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/units"
)

// SettingsScreen is which settings screen is showing.
type SettingsScreen int

const (
	SettingsRoot       SettingsScreen = iota // the list: units preset, then one row per metric
	SettingsPickPreset                       // choose metric / imperial
	SettingsPickUnit                         // choose a unit for View.Metric
)

// SettingsView is the dialog's navigation state. The app owns it; drawing
// and tap handling are pure functions of it, so both are easy to test.
type SettingsView struct {
	Screen SettingsScreen
	Metric string // which metric SettingsPickUnit is for
}

type ActionKind int

const (
	ActNone             ActionKind = iota
	ActBack                        // up one level; from the root, close settings
	ActOpenPresetPicker            // root -> preset picker
	ActOpenUnitPicker              // root -> unit picker for Action.Metric
	ActSetPreset                   // apply Action.Value as the preset
	ActSetUnit                     // apply Action.Value as Action.Metric's unit
)

// Action is what a tap on the settings screen asks the app to do.
type Action struct {
	Kind          ActionKind
	Metric, Value string
}

const (
	settingsRowH = 120
	settingsTop  = headerH + 30
)

// unitNames spells out the unit symbols that aren't obvious on a boat.
var unitNames = map[string]string{
	"kn": "knots", "km/h": "kilometres per hour", "mph": "miles per hour", "m/s": "metres per second",
	"m": "metres", "ft": "feet", "fm": "fathoms", "nm": "nautical miles", "km": "kilometres", "mi": "miles",
}

func metricByID(id string) (units.Metric, bool) {
	for _, m := range units.Metrics {
		if m.ID == id {
			return m, true
		}
	}
	return units.Metric{}, false
}

func titleCase(s string) string {
	if s == "" {
		return s
	}
	return strings.ToUpper(s[:1]) + s[1:]
}

// SettingsRowY is the vertical middle of list row i, for callers (and
// tests) that need to tap a particular row.
func SettingsRowY(i int) int { return settingsTop + i*settingsRowH + settingsRowH/2 }

// rowAt maps a y coordinate to a list row index, or -1.
func rowAt(y int) int {
	if y < settingsTop {
		return -1
	}
	return (y - settingsTop) / settingsRowH
}

// SettingsTap works out what a tap at (x, y) means on the given screen.
func SettingsTap(v SettingsView, x, y int) Action {
	if image.Pt(x, y).In(CogRect) {
		return Action{Kind: ActBack}
	}
	row := rowAt(y)
	switch v.Screen {
	case SettingsRoot:
		if row == 0 {
			return Action{Kind: ActOpenPresetPicker}
		}
		if row >= 1 && row <= len(units.Metrics) {
			return Action{Kind: ActOpenUnitPicker, Metric: units.Metrics[row-1].ID}
		}
	case SettingsPickPreset:
		if p := units.Presets(); row >= 0 && row < len(p) {
			return Action{Kind: ActSetPreset, Value: p[row]}
		}
	case SettingsPickUnit:
		if m, ok := metricByID(v.Metric); ok {
			if opts := units.Units(m.Qty); row >= 0 && row < len(opts) {
				return Action{Kind: ActSetUnit, Metric: m.ID, Value: opts[row].Symbol}
			}
		}
	}
	return Action{}
}

func chevronLeft(c *render.Canvas, x, y, h int, shade uint8) {
	c.Line(float64(x+h), float64(y-h), float64(x), float64(y), 8, shade)
	c.Line(float64(x), float64(y), float64(x+h), float64(y+h), 8, shade)
}

func chevronRight(c *render.Canvas, x, y, h int, shade uint8) {
	c.Line(float64(x-h), float64(y-h), float64(x), float64(y), 7, shade)
	c.Line(float64(x), float64(y), float64(x-h), float64(y+h), 7, shade)
}

// row draws one list row: a label on the left and whatever the caller
// draws on the right.
func row(c *render.Canvas, i int, label string, right func(cy int)) {
	b := c.Bounds()
	y0 := settingsTop + i*settingsRowH
	cy := y0 + settingsRowH/2
	c.Text(40, cy+16, label, 48, render.Regular, render.Left, render.Black)
	if right != nil {
		right(cy)
	}
	c.HLine(40, b.Dx()-40, y0+settingsRowH-2, 2, render.Mid)
}

// Settings draws the settings screens.
func Settings(c *render.Canvas, v SettingsView, u units.Settings) {
	b := c.Bounds()

	title := "SETTINGS"
	switch v.Screen {
	case SettingsPickPreset:
		title = "UNITS"
	case SettingsPickUnit:
		if m, ok := metricByID(v.Metric); ok {
			title = strings.ToUpper(m.Label)
		}
	}
	// A back chevron where the cog is on other pages, in the same tap area.
	chevronLeft(c, 38, int(cogY), 22, render.Black)
	c.Text(titleX, 64, title, 56, render.Bold, render.Left, render.Black)
	c.HLine(0, b.Dx(), headerH, 4, render.Black)

	switch v.Screen {
	case SettingsRoot:
		row(c, 0, "Units", func(cy int) {
			c.Text(b.Dx()-110, cy+16, titleCase(u.Preset), 50, render.Bold, render.Right, render.Black)
			chevronRight(c, b.Dx()-50, cy, 18, render.Dark)
		})
		for i, m := range units.Metrics {
			m := m
			row(c, i+1, m.Label, func(cy int) {
				sym := u.UnitFor(m.ID).Symbol
				if u.IsOverridden(m.ID) {
					sym += " *"
				}
				c.Text(b.Dx()-110, cy+16, sym, 50, render.Bold, render.Right, render.Black)
				chevronRight(c, b.Dx()-50, cy, 18, render.Dark)
			})
		}
		c.Text(40, settingsTop+(len(units.Metrics)+1)*settingsRowH+60,
			"* set individually, not from the preset", 36, render.Regular, render.Left, render.Dark)

	case SettingsPickPreset:
		for i, p := range units.Presets() {
			p := p
			row(c, i, titleCase(p), func(cy int) { radio(c, b.Dx()-80, cy, u.Preset == p) })
		}
		c.Text(40, settingsTop+len(units.Presets())*settingsRowH+60,
			"Choosing a preset resets every unit to it", 36, render.Regular, render.Left, render.Dark)

	case SettingsPickUnit:
		m, ok := metricByID(v.Metric)
		if !ok {
			return
		}
		current := u.UnitFor(m.ID).Symbol
		for i, opt := range units.Units(m.Qty) {
			label := opt.Symbol
			if n, ok := unitNames[opt.Symbol]; ok {
				label += "  " + n
			}
			sym := opt.Symbol
			row(c, i, label, func(cy int) { radio(c, b.Dx()-80, cy, current == sym) })
		}
	}
}

// radio draws a radio button, filled when selected.
func radio(c *render.Canvas, x, y int, selected bool) {
	c.Ring(float64(x), float64(y), 26, 6, render.Black)
	if selected {
		c.Disc(float64(x), float64(y), 14, render.Black)
	}
}
