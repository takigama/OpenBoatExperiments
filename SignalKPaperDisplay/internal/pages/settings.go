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
	SettingsBoxes                            // the six Nav boxes and what each shows
	SettingsPickBox                          // choose what box View.Box shows
)

// Version is the running release number, shown at the foot of the settings
// list. main sets it from the build stamp.
var Version = "0"

// SettingsView is the dialog's navigation state. The app owns it; drawing
// and tap handling are pure functions of it, so both are easy to test.
type SettingsView struct {
	Screen SettingsScreen
	Metric string // which metric SettingsPickUnit is for
	Box    int    // which Nav box SettingsPickBox is for, 0..NavBoxes-1
}

type ActionKind int

const (
	ActNone             ActionKind = iota
	ActBack                        // up one level; from the root, close settings
	ActOpenPresetPicker            // root -> preset picker
	ActOpenUnitPicker              // root -> unit picker for Action.Metric
	ActSetPreset                   // apply Action.Value as the preset
	ActSetUnit                     // apply Action.Value as Action.Metric's unit
	ActToggleInvert                // flip white-on-black / black-on-white
	ActOpenBoxes                   // root -> the list of Nav boxes
	ActOpenBoxPicker               // boxes -> what-to-show picker for Action.Box
	ActSetBox                      // box Action.Box shows kind Action.Value
)

// Action is what a tap on the settings screen asks the app to do.
type Action struct {
	Kind          ActionKind
	Metric, Value string
	Box           int
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

// invertRow is the list row of the invert-colours toggle: after the units
// preset (row 0) and one row per metric, so adding a metric moves it down
// by itself.
func invertRow() int { return len(units.Metrics) + 1 }

// boxesRow is the list row that opens the Nav box layout, just after invert.
func boxesRow() int { return invertRow() + 1 }

// boxPositions names the Nav boxes for the list, in box order.
var boxPositions = [NavBoxes]string{
	"Top left", "Top right", "Middle left", "Middle right", "Bottom left", "Bottom right",
}

// ParentScreen is where Back goes from a screen: pickers return to the list
// they came from; everything else returns to the root.
func ParentScreen(s SettingsScreen) SettingsScreen {
	if s == SettingsPickBox {
		return SettingsBoxes
	}
	return SettingsRoot
}

// pickerCols is how many columns the box-kind picker has: there are too many
// kinds for one column of tappable rows.
const pickerCols = 2

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

// SettingsTap works out what a tap at (x, y) means on the given screen,
// which is width pixels wide.
func SettingsTap(v SettingsView, x, y, width int) Action {
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
		if row == invertRow() {
			return Action{Kind: ActToggleInvert}
		}
		if row == boxesRow() {
			return Action{Kind: ActOpenBoxes}
		}
	case SettingsBoxes:
		if row >= 0 && row < NavBoxes {
			return Action{Kind: ActOpenBoxPicker, Box: row}
		}
	case SettingsPickBox:
		col := x * pickerCols / width
		if i := row*pickerCols + col; row >= 0 && col >= 0 && col < pickerCols && i < len(BoxKinds) {
			return Action{Kind: ActSetBox, Box: v.Box, Value: BoxKinds[i].ID}
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

// Settings draws the settings screens. invert is the current state of the
// invert-colours option, shown on its row; boxes is what each Nav box shows.
func Settings(c *render.Canvas, v SettingsView, u units.Settings, invert bool, boxes []string) {
	b := c.Bounds()

	title := "SETTINGS"
	switch v.Screen {
	case SettingsPickPreset:
		title = "UNITS"
	case SettingsPickUnit:
		if m, ok := metricByID(v.Metric); ok {
			title = strings.ToUpper(m.Label)
		}
	case SettingsBoxes:
		title = "NAV BOXES"
	case SettingsPickBox:
		title = strings.ToUpper(boxPositions[clampBox(v.Box)])
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
		row(c, invertRow(), "Invert colours", func(cy int) {
			state := "Off"
			if invert {
				state = "On"
			}
			c.Text(b.Dx()-50, cy+16, state, 50, render.Bold, render.Right, render.Black)
		})
		row(c, boxesRow(), "Nav boxes", func(cy int) { chevronRight(c, b.Dx()-50, cy, 18, render.Dark) })
		c.Text(40, settingsTop+(boxesRow()+1)*settingsRowH+60,
			"* set individually, not from the preset", 36, render.Regular, render.Left, render.Dark)
		// Which build this is, so it's plain from the screen that an update
		// has actually been picked up (a replaced file isn't running until
		// the app restarts).
		c.Text(b.Dx()-40, b.Dy()-40, "v"+Version, 36, render.Regular, render.Right, render.Dark)

	case SettingsBoxes:
		kinds := NormalizeBoxes(boxes)
		for i := 0; i < NavBoxes; i++ {
			k, _ := BoxKindByID(kinds[i])
			row(c, i, boxPositions[i], func(cy int) {
				c.Text(b.Dx()-110, cy+16, k.Name, 44, render.Bold, render.Right, render.Black)
				chevronRight(c, b.Dx()-50, cy, 18, render.Dark)
			})
		}

	case SettingsPickBox:
		current := NormalizeBoxes(boxes)[clampBox(v.Box)]
		colW := b.Dx() / pickerCols
		for i, k := range BoxKinds {
			x0 := (i % pickerCols) * colW
			y0 := settingsTop + (i/pickerCols)*settingsRowH
			cy := y0 + settingsRowH/2
			radio(c, x0+56, cy, k.ID == current)
			c.Text(x0+100, cy+14, fitText(c, k.Name, 38, render.Regular, colW-120), 38, render.Regular, render.Left, render.Black)
			c.HLine(x0+30, x0+colW-30, y0+settingsRowH-2, 2, render.Mid)
		}

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

func clampBox(i int) int { return max(0, min(NavBoxes-1, i)) }

// radio draws a radio button, filled when selected.
func radio(c *render.Canvas, x, y int, selected bool) {
	c.Ring(float64(x), float64(y), 26, 6, render.Black)
	if selected {
		c.Disc(float64(x), float64(y), 14, render.Black)
	}
}
