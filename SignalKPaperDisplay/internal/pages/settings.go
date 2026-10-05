package pages

import (
	"image"
	"strconv"
	"strings"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/units"
)

// SettingsScreen is which settings screen is showing.
type SettingsScreen int

const (
	SettingsRoot         SettingsScreen = iota // the list: units preset, then one row per metric
	SettingsPickPreset                         // choose metric / imperial
	SettingsPickUnit                           // choose a unit for View.Metric
	SettingsBoxes                              // the six Nav boxes and what each shows
	SettingsPickBox                            // choose what box View.Box shows
	SettingsServer                             // type in the SignalK server address
	SettingsLight                              // set the front light brightness
	SettingsPower                              // switch off, restart, or back to the Kindle's own software
	SettingsPowerConfirm                       // are you sure about View.Power
	SettingsNoPower                            // choose how long off power before the NO POWER screen
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
	Page   int    // SettingsPickBox: which page of kinds is showing
	Text   string // SettingsServer: the address typed so far
	Err    string // SettingsServer: why the last Save was refused

	// The front light, filled in by the app each time the view is used (it is
	// not navigation state): its current level and maximum. A zero MaxLevel
	// means the device has no controllable light, and the setting is hidden.
	Level, MaxLevel int

	// Demo is whether demo mode is on, filled in by the app like the light.
	Demo bool

	Power string // SettingsPowerConfirm: which of the PowerChoices is being confirmed

	// NoPower is the no-power timeout in minutes (0 is never), filled in by the app.
	NoPower int
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
	ActBoxPage                     // show page Action.Page of the box picker
	ActOpenServer                  // root -> the server address editor
	ActServerKey                   // a keypad key, Action.Value: a character, "back", "clear" or "save"
	ActOpenLight                   // root -> the front light screen
	ActSetLight                    // set the front light to Action.Level
	ActToggleDemo                  // switch demo mode on or off
	ActOpenPower                   // root -> the power screen
	ActPowerPick                   // power screen -> confirm Action.Value
	ActPowerDo                     // do Action.Value (a power choice), confirmed
	ActOpenNoPower                 // power screen -> the no-power timeout picker
	ActSetNoPower                  // set the no-power timeout to Action.Level minutes (0: never)
)

// Action is what a tap on the settings screen asks the app to do.
type Action struct {
	Kind          ActionKind
	Metric, Value string
	Box           int
	Level         int
	Page          int
}

const (
	settingsRowH = 84 // the list's rows must all fit on the tallest screen, with the notes under them
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

// serverRow is the list row that opens the server address editor.
func serverRow() int { return boxesRow() + 1 }

// lightRow is the list row that opens the front light screen, after the
// server row; it only exists on devices with a controllable light.
func lightRow() int { return serverRow() + 1 }

// demoRow is the last row of the list, the demo mode switch: after the server
// row, and after the front light's too on devices that have one.
func demoRow(hasLight bool) int {
	if hasLight {
		return lightRow() + 1
	}
	return serverRow() + 1
}

// powerRow is the very last row, opening the power screen.
func powerRow(hasLight bool) int { return demoRow(hasLight) + 1 }

// The front light screen: a bar you tap to pick a level, minus and plus
// buttons, and off and maximum buttons.
var (
	lightBar    = image.Rect(60, 340, 1012, 460)
	lightMinus  = image.Rect(60, 540, 520, 700)
	lightPlus   = image.Rect(552, 540, 1012, 700)
	lightOff    = image.Rect(60, 740, 520, 900)
	lightMaxBtn = image.Rect(552, 740, 1012, 900)
)

// lightTap is what a tap at pt does on the front light screen.
func lightTap(v SettingsView, pt image.Point) Action {
	if v.MaxLevel <= 0 {
		return Action{}
	}
	set := func(n int) Action { return Action{Kind: ActSetLight, Level: max(0, min(n, v.MaxLevel))} }
	switch {
	case pt.In(lightBar.Inset(-40)): // a generous target: a fingertip is not a pixel
		x := max(lightBar.Min.X, min(pt.X, lightBar.Max.X))
		return set(((x-lightBar.Min.X)*v.MaxLevel + lightBar.Dx()/2) / lightBar.Dx()) // rounded
	case pt.In(lightMinus):
		return set(v.Level - 1)
	case pt.In(lightPlus):
		return set(v.Level + 1)
	case pt.In(lightOff):
		return set(0)
	case pt.In(lightMaxBtn):
		return set(v.MaxLevel)
	}
	return Action{}
}

// The server address keypad: three keys across, filled row by row. An
// address is only digits, dots and a colon, so that's all the keys there are.
var serverKeys = [15]struct{ ID, Label string }{
	{"1", "1"}, {"2", "2"}, {"3", "3"},
	{"4", "4"}, {"5", "5"}, {"6", "6"},
	{"7", "7"}, {"8", "8"}, {"9", "9"},
	{".", "."}, {"0", "0"}, {":", ":"},
	{"back", ""}, {"clear", "CLR"}, {"save", "SAVE"},
}

const (
	keypadCols   = 3
	keypadTop    = 380
	keypadKeyH   = 180
	maxServerLen = 40
)

// ApplyServerKey returns the address text after a keypad key: characters are
// added (up to a limit), "back" deletes the last one, "clear" empties it.
func ApplyServerKey(text, key string) string {
	switch key {
	case "back":
		if r := []rune(text); len(r) > 0 {
			return string(r[:len(r)-1])
		}
		return text
	case "clear":
		return ""
	}
	if len(text) >= maxServerLen {
		return text
	}
	return text + key
}

// boxPositions names the Nav boxes for the list, in box order.
var boxPositions = [NavBoxes]string{
	"Top left", "Top right", "Second left", "Second right",
	"Third left", "Third right", "Bottom left", "Bottom right",
}

// ParentScreen is where Back goes from a screen: pickers return to the list
// they came from; everything else returns to the root.
func ParentScreen(s SettingsScreen) SettingsScreen {
	if s == SettingsPickBox {
		return SettingsBoxes
	}
	if s == SettingsPowerConfirm || s == SettingsNoPower {
		return SettingsPower
	}
	return SettingsRoot
}

// The box picker pages kinds BoxesPerPicker at a time; these two buttons turn
// the page, under the grid.
var (
	pickerPrev = image.Rect(40, 1110, 360, 1230)
	pickerNext = image.Rect(712, 1110, 1032, 1230)
)

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
		if row == serverRow() {
			return Action{Kind: ActOpenServer}
		}
		if v.MaxLevel > 0 && row == lightRow() {
			return Action{Kind: ActOpenLight}
		}
		if row == demoRow(v.MaxLevel > 0) {
			return Action{Kind: ActToggleDemo}
		}
		if row == powerRow(v.MaxLevel > 0) {
			return Action{Kind: ActOpenPower}
		}
	case SettingsPower, SettingsPowerConfirm:
		return powerTap(v, image.Pt(x, y), width)
	case SettingsNoPower:
		if row >= 0 && row < len(NoPowerChoices) {
			return Action{Kind: ActSetNoPower, Level: NoPowerChoices[row].Minutes}
		}
	case SettingsLight:
		return lightTap(v, image.Pt(x, y))
	case SettingsServer:
		if y >= keypadTop && width > 0 {
			col, r := x*keypadCols/width, (y-keypadTop)/keypadKeyH
			if i := r*keypadCols + col; col >= 0 && col < keypadCols && i < len(serverKeys) {
				return Action{Kind: ActServerKey, Value: serverKeys[i].ID}
			}
		}
	case SettingsBoxes:
		if row >= 0 && row < NavBoxes {
			return Action{Kind: ActOpenBoxPicker, Box: row}
		}
	case SettingsPickBox:
		pages := BoxPages()
		page := max(0, min(v.Page, pages-1))
		if pages > 1 {
			if image.Pt(x, y).In(pickerPrev) && page > 0 {
				return Action{Kind: ActBoxPage, Page: page - 1}
			}
			if image.Pt(x, y).In(pickerNext) && page < pages-1 {
				return Action{Kind: ActBoxPage, Page: page + 1}
			}
		}
		col := x * pickerCols / width
		if i := page*BoxesPerPicker + row*pickerCols + col; row >= 0 && col >= 0 && col < pickerCols &&
			row*pickerCols+col < BoxesPerPicker && i < len(BoxKinds) {
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
// invert-colours option, shown on its row; boxes is what each Nav box shows;
// server is the SignalK address in use, shown on its row.
func Settings(c *render.Canvas, v SettingsView, u units.Settings, invert bool, boxes []string, server string) {
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
	case SettingsServer:
		title = "SIGNALK SERVER"
	case SettingsLight:
		title = "BACKLIGHT"
	case SettingsPower, SettingsPowerConfirm:
		title = "POWER"
	case SettingsNoPower:
		title = "NO-POWER MODE"
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
		row(c, serverRow(), "SignalK server", func(cy int) {
			c.Text(b.Dx()-110, cy+16, server, 44, render.Bold, render.Right, render.Black)
			chevronRight(c, b.Dx()-50, cy, 18, render.Dark)
		})
		if v.MaxLevel > 0 {
			row(c, lightRow(), "Backlight", func(cy int) {
				c.Text(b.Dx()-110, cy+16, lightLabel(v.Level, v.MaxLevel), 44, render.Bold, render.Right, render.Black)
				chevronRight(c, b.Dx()-50, cy, 18, render.Dark)
			})
		}
		row(c, demoRow(v.MaxLevel > 0), "Demo mode", func(cy int) {
			state := "Off"
			if v.Demo {
				state = "On"
			}
			c.Text(b.Dx()-50, cy+16, state, 50, render.Bold, render.Right, render.Black)
		})
		row(c, powerRow(v.MaxLevel > 0), "Power", func(cy int) { chevronRight(c, b.Dx()-50, cy, 18, render.Dark) })
		footRow := powerRow(v.MaxLevel > 0) + 1
		c.Text(40, settingsTop+footRow*settingsRowH+60,
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

	case SettingsServer:
		drawServerEditor(c, v)

	case SettingsLight:
		drawLight(c, v)

	case SettingsPower, SettingsPowerConfirm:
		drawPower(c, v)

	case SettingsNoPower:
		for i, ch := range NoPowerChoices {
			min := ch.Minutes
			row(c, i, ch.Label, func(cy int) { radio(c, b.Dx()-80, cy, v.NoPower == min) })
		}
		for i, l := range []string{
			"After this long off external power the screen shows",
			"NO POWER and stops updating, to save the battery.",
			"Plugging in, a tap or the power button brings it back.",
		} {
			c.Text(40, settingsTop+len(NoPowerChoices)*settingsRowH+60+i*46, l, 36, render.Regular, render.Left, render.Dark)
		}

	case SettingsPickBox:
		current := NormalizeBoxes(boxes)[clampBox(v.Box)]
		colW := b.Dx() / pickerCols
		pages := BoxPages()
		page := max(0, min(v.Page, pages-1))
		for j := 0; j < BoxesPerPicker; j++ {
			i := page*BoxesPerPicker + j
			if i >= len(BoxKinds) {
				break
			}
			k := BoxKinds[i]
			x0 := (j % pickerCols) * colW
			y0 := settingsTop + (j/pickerCols)*settingsRowH
			cy := y0 + settingsRowH/2
			radio(c, x0+56, cy, k.ID == current)
			c.Text(x0+100, cy+14, fitText(c, k.Name, 38, render.Regular, colW-120), 38, render.Regular, render.Left, render.Black)
			c.HLine(x0+30, x0+colW-30, y0+settingsRowH-2, 2, render.Mid)
		}
		if pages > 1 {
			button := func(r image.Rectangle, label string, on bool) {
				c.FillRect(r, render.Black)
				c.FillRect(r.Inset(4), render.White)
				shade := render.Black
				if !on {
					shade = render.Mid // nowhere to go that way
				}
				c.Text((r.Min.X+r.Max.X)/2, (r.Min.Y+r.Max.Y)/2+18, label, 52, render.Bold, render.Center, shade)
			}
			button(pickerPrev, "< PREV", page > 0)
			button(pickerNext, "NEXT >", page < pages-1)
			c.Text(b.Dx()/2, (pickerPrev.Min.Y+pickerPrev.Max.Y)/2+16, strconv.Itoa(page+1)+" / "+strconv.Itoa(pages), 44, render.Bold, render.Center, render.Black)
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

// lightLabel is how a level reads in the list: "Off", or "12 / 24".
func lightLabel(level, max int) string {
	if level <= 0 {
		return "Off"
	}
	return strconv.Itoa(level) + " / " + strconv.Itoa(max)
}

// drawLight draws the front light screen. Only black and white are used: it is
// redrawn in place on every tap, under the fast waveform.
func drawLight(c *render.Canvas, v SettingsView) {
	b := c.Bounds()
	level := max(0, min(v.Level, v.MaxLevel))
	c.Text(b.Dx()/2, 270, strconv.Itoa(level), 170, render.Bold, render.Center, render.Black)
	c.Text(b.Dx()/2, 318, "of "+strconv.Itoa(v.MaxLevel), 40, render.Regular, render.Center, render.Black)

	c.FillRect(lightBar, render.Black)
	inner := lightBar.Inset(5)
	c.FillRect(inner, render.White)
	if v.MaxLevel > 0 {
		w := inner.Dx() * level / v.MaxLevel
		c.FillRect(image.Rect(inner.Min.X, inner.Min.Y, inner.Min.X+w, inner.Max.Y), render.Black)
	}

	button := func(r image.Rectangle, label string, size float64) {
		c.FillRect(r, render.Black)
		c.FillRect(r.Inset(5), render.White)
		c.Text((r.Min.X+r.Max.X)/2, (r.Min.Y+r.Max.Y)/2+int(size*0.34), label, size, render.Bold, render.Center, render.Black)
	}
	button(lightMinus, "-", 130)
	button(lightPlus, "+", 130)
	button(lightOff, "OFF", 64)
	button(lightMaxBtn, "MAX", 64)
}

// drawServerEditor draws the address typed so far above a numeric keypad.
func drawServerEditor(c *render.Canvas, v SettingsView) {
	b := c.Bounds()
	field := image.Rect(40, 130, b.Dx()-40, 250)
	c.FillRect(field, render.Black)
	c.FillRect(field.Inset(4), render.White)
	c.Text(64, field.Max.Y-34, v.Text+"_", 64, render.Bold, render.Left, render.Black)
	hint, shade := "an IP address and port, like 192.168.1.20:3000", render.Dark
	if v.Err != "" {
		hint, shade = v.Err, render.Black
	}
	c.Text(46, 310, hint, 36, render.Regular, render.Left, shade)

	w := b.Dx() / keypadCols
	for i, k := range serverKeys {
		x0 := (i % keypadCols) * w
		y0 := keypadTop + (i/keypadCols)*keypadKeyH
		key := image.Rect(x0+12, y0+12, x0+w-12, y0+keypadKeyH-12)
		fg := render.Black
		c.FillRect(key, render.Black)
		if k.ID == "save" {
			fg = render.White // the one action that commits: solid
		} else {
			c.FillRect(key.Inset(4), render.White)
		}
		cx, cy := (key.Min.X+key.Max.X)/2, (key.Min.Y+key.Max.Y)/2
		if k.ID == "back" {
			// A backspace arrow: a left-pointing shaft and head.
			c.Line(float64(cx-44), float64(cy), float64(cx+44), float64(cy), 8, fg)
			c.Line(float64(cx-44), float64(cy), float64(cx-8), float64(cy-34), 8, fg)
			c.Line(float64(cx-44), float64(cy), float64(cx-8), float64(cy+34), 8, fg)
			continue
		}
		size := 84.0
		if len(k.Label) > 1 {
			size = 56
		}
		c.Text(cx, cy+int(size*0.34), k.Label, size, render.Bold, render.Center, fg)
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
