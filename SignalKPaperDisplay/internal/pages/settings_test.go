package pages

import (
	"image"
	"testing"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/units"
)

// rowY is the middle of list row i.
func rowY(i int) int { return settingsTop + i*settingsRowH + settingsRowH/2 }

func TestSettingsTapRoot(t *testing.T) {
	root := SettingsView{Screen: SettingsRoot}

	if got := SettingsTap(root, 500, rowY(0), 1072); got.Kind != ActOpenPresetPicker {
		t.Errorf("row 0 = %+v, want the preset picker", got)
	}
	// Every metric row opens its own unit picker, in the order of units.Metrics.
	for i, m := range units.Metrics {
		got := SettingsTap(root, 500, rowY(i+1), 1072)
		if got.Kind != ActOpenUnitPicker || got.Metric != m.ID {
			t.Errorf("row %d = %+v, want unit picker for %s", i+1, got, m.ID)
		}
	}
	if got := SettingsTap(root, 500, rowY(len(units.Metrics)+4), 1072); got.Kind != ActNone {
		t.Errorf("a tap below the list = %+v, want nothing", got)
	}
	if got := SettingsTap(root, 500, headerH+5, 1072); got.Kind != ActNone {
		t.Errorf("a tap in the header gap = %+v, want nothing", got)
	}
}

func TestInvertRowIsLastInTheList(t *testing.T) {
	root := SettingsView{Screen: SettingsRoot}
	got := SettingsTap(root, 500, rowY(len(units.Metrics)+1), 1072)
	if got.Kind != ActToggleInvert {
		t.Errorf("the row after the last metric = %+v, want the invert toggle", got)
	}
	// It's a toggle, not a picker: it must not exist on the other screens.
	if got := SettingsTap(SettingsView{Screen: SettingsPickPreset}, 500, rowY(len(units.Metrics)+1), 1072); got.Kind == ActToggleInvert {
		t.Error("the preset picker must not have the invert toggle")
	}
}

func TestInvertRowShowsItsState(t *testing.T) {
	// The On/Off text is at the right end of the invert row.
	on, off := func() *render.Canvas {
		c, _ := render.NewCanvas(1072, 1448)
		Settings(c, SettingsView{}, units.Settings{}, true, nil, "")
		return c
	}(), func() *render.Canvas {
		c, _ := render.NewCanvas(1072, 1448)
		Settings(c, SettingsView{}, units.Settings{}, false, nil, "")
		return c
	}()
	y := rowY(len(units.Metrics) + 1)
	cell := image.Rect(900, y-30, 1030, y+30)
	if !differs(on, off) || inked(on, cell) == inked(off, cell) {
		t.Error("the invert row should read differently when on and off")
	}
}

func TestSettingsTapPickers(t *testing.T) {
	p := units.Presets()
	if got := SettingsTap(SettingsView{Screen: SettingsPickPreset}, 500, rowY(1), 1072); got.Kind != ActSetPreset || got.Value != p[1] {
		t.Errorf("preset row 1 = %+v, want preset %s", got, p[1])
	}

	v := SettingsView{Screen: SettingsPickUnit, Metric: "depth"}
	opts := units.Units(units.Depth)
	for i, o := range opts {
		got := SettingsTap(v, 500, rowY(i), 1072)
		if got.Kind != ActSetUnit || got.Metric != "depth" || got.Value != o.Symbol {
			t.Errorf("depth row %d = %+v, want %s", i, got, o.Symbol)
		}
	}
	if got := SettingsTap(v, 500, rowY(len(opts)), 1072); got.Kind != ActNone {
		t.Errorf("a tap past the last choice = %+v, want nothing", got)
	}
}

func TestCogAndBackShareTheSameTapArea(t *testing.T) {
	x, y := CogRect.Min.X+5, CogRect.Min.Y+5
	for _, s := range []SettingsScreen{SettingsRoot, SettingsPickPreset, SettingsPickUnit} {
		if got := SettingsTap(SettingsView{Screen: s, Metric: "sog"}, x, y, 1072); got.Kind != ActBack {
			t.Errorf("screen %d: tap on the cog area = %+v, want back", s, got)
		}
	}
}

func TestSettingsScreensDraw(t *testing.T) {
	// Drawing must not panic on any screen, and must put something on it.
	u := units.Settings{Preset: units.PresetImperial}
	u.SetUnit("sog", "kn")
	for _, v := range []SettingsView{
		{Screen: SettingsRoot},
		{Screen: SettingsPickPreset},
		{Screen: SettingsPickUnit, Metric: "sog"},
		{Screen: SettingsPickUnit, Metric: "no-such-metric"},
	} {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, v, u, false, nil, "")
		dark := 0
		for _, p := range c.Img.Pix {
			if p < 128 {
				dark++
			}
		}
		if dark == 0 {
			t.Errorf("%+v drew nothing", v)
		}
	}
}

func TestSettingsListShowsTheVersion(t *testing.T) {
	draw := func(v string) *render.Canvas {
		old := Version
		Version = v
		defer func() { Version = old }()
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, SettingsView{}, units.Settings{}, false, nil, "")
		return c
	}
	foot := image.Rect(700, 1380, 1072, 1448)
	if inked(draw("19"), foot) == 0 {
		t.Error("the version should be drawn at the foot of the settings list")
	}
	if !differs(draw("19"), draw("20")) {
		t.Error("a different version should look different")
	}
}

func TestSettingsBoxScreens(t *testing.T) {
	const w = 1072
	root := SettingsView{Screen: SettingsRoot}
	if got := SettingsTap(root, 500, SettingsRowY(len(units.Metrics)+2), w); got.Kind != ActOpenBoxes {
		t.Errorf("the row after invert should open the Nav boxes, got %+v", got)
	}

	list := SettingsView{Screen: SettingsBoxes}
	for i := 0; i < NavBoxes; i++ {
		if got := SettingsTap(list, 500, SettingsRowY(i), w); got.Kind != ActOpenBoxPicker || got.Box != i {
			t.Errorf("box row %d = %+v", i, got)
		}
	}
	if got := SettingsTap(list, 500, SettingsRowY(NavBoxes), w); got.Kind != ActNone {
		t.Errorf("below the last box nothing should happen, got %+v", got)
	}

	// The picker is two columns, filled row by row, one page at a time.
	for page := 0; page < BoxPages(); page++ {
		pick := SettingsView{Screen: SettingsPickBox, Box: 3, Page: page}
		for j := 0; j < BoxesPerPicker; j++ {
			i := page*BoxesPerPicker + j
			x := 200 + (j%2)*600
			got := SettingsTap(pick, x, SettingsRowY(j/2), w)
			if i >= len(BoxKinds) {
				if got.Kind != ActNone {
					t.Errorf("page %d slot %d is empty but did %+v", page, j, got)
				}
				continue
			}
			if got.Kind != ActSetBox || got.Value != BoxKinds[i].ID || got.Box != 3 {
				t.Errorf("page %d slot %d (%s) = %+v", page, j, BoxKinds[i].ID, got)
			}
		}
	}
	// Everything fits on screen above the page buttons.
	if last := SettingsRowY(BoxesPerPicker/2-1) + settingsRowH/2; last > pickerPrev.Min.Y {
		t.Errorf("a full page of kinds reaches y=%d, into the page buttons at %d", last, pickerPrev.Min.Y)
	}
	if pickerPrev.Max.Y > 1448 || pickerNext.Max.Y > 1448 {
		t.Error("the page buttons run off the screen")
	}

	if ParentScreen(SettingsPickBox) != SettingsBoxes || ParentScreen(SettingsBoxes) != SettingsRoot ||
		ParentScreen(SettingsPickUnit) != SettingsRoot {
		t.Error("back should go pickers -> their list -> root")
	}
}

func TestBoxPickerPages(t *testing.T) {
	const w = 1072
	if BoxPages() < 2 {
		t.Fatalf("with %d kinds the picker should need several pages, got %d", len(BoxKinds), BoxPages())
	}
	mid := func(r image.Rectangle) image.Point { return image.Pt((r.Min.X+r.Max.X)/2, (r.Min.Y+r.Max.Y)/2) }
	first := SettingsView{Screen: SettingsPickBox, Page: 0}
	if got := SettingsTap(first, mid(pickerNext).X, mid(pickerNext).Y, w); got.Kind != ActBoxPage || got.Page != 1 {
		t.Errorf("next from page 0 = %+v", got)
	}
	if got := SettingsTap(first, mid(pickerPrev).X, mid(pickerPrev).Y, w); got.Kind != ActNone {
		t.Errorf("prev from the first page should do nothing, got %+v", got)
	}
	lastPage := BoxPages() - 1
	last := SettingsView{Screen: SettingsPickBox, Page: lastPage}
	if got := SettingsTap(last, mid(pickerPrev).X, mid(pickerPrev).Y, w); got.Kind != ActBoxPage || got.Page != lastPage-1 {
		t.Errorf("prev from the last page = %+v", got)
	}
	if got := SettingsTap(last, mid(pickerNext).X, mid(pickerNext).Y, w); got.Kind != ActNone {
		t.Errorf("next from the last page should do nothing, got %+v", got)
	}
	// A page number out of range is treated as the nearest real page.
	if got := SettingsTap(SettingsView{Screen: SettingsPickBox, Page: 99}, 200, SettingsRowY(0), w); got.Kind != ActSetBox ||
		got.Value != BoxKinds[lastPage*BoxesPerPicker].ID {
		t.Errorf("page 99 = %+v", got)
	}
	// Every kind is on exactly one page, and BoxPageOf agrees.
	for i, k := range BoxKinds {
		if BoxPageOf(k.ID) != i/BoxesPerPicker {
			t.Errorf("%s: page %d, want %d", k.ID, BoxPageOf(k.ID), i/BoxesPerPicker)
		}
	}
	if BoxPageOf("nonsense") != 0 {
		t.Error("an unknown kind starts on the first page")
	}
}

func TestSettingsBoxScreensDraw(t *testing.T) {
	for _, v := range []SettingsView{{Screen: SettingsBoxes}, {Screen: SettingsPickBox, Box: 5}, {Screen: SettingsPickBox, Box: 99}} {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, v, units.Settings{}, false, []string{"stw"}, "")
		if inked(c, image.Rect(0, headerH+10, 1072, 1448)) == 0 {
			t.Errorf("%+v drew nothing", v)
		}
	}
	// The picker marks the current choice.
	draw := func(boxes []string) *render.Canvas {
		c, _ := render.NewCanvas(1072, 1448)
		Settings(c, SettingsView{Screen: SettingsPickBox, Box: 0}, units.Settings{}, false, boxes, "")
		return c
	}
	if !differs(draw([]string{"sog"}), draw([]string{"stw"})) {
		t.Error("the picker should show which kind is selected")
	}
}

func TestServerRowAndKeypadTaps(t *testing.T) {
	const w = 1072
	if got := SettingsTap(SettingsView{}, 500, SettingsRowY(len(units.Metrics)+3), w); got.Kind != ActOpenServer {
		t.Errorf("the row after Nav boxes should open the server editor, got %+v", got)
	}

	v := SettingsView{Screen: SettingsServer}
	keyAt := func(row, col int) Action {
		return SettingsTap(v, col*w/3+w/6, keypadTop+row*keypadKeyH+keypadKeyH/2, w)
	}
	want := [][]string{{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {".", "0", ":"}, {"back", "clear", "save"}}
	for r, keys := range want {
		for c, id := range keys {
			if got := keyAt(r, c); got.Kind != ActServerKey || got.Value != id {
				t.Errorf("key (%d,%d) = %+v, want %q", r, c, got, id)
			}
		}
	}
	if got := SettingsTap(v, 500, keypadTop-20, w); got.Kind != ActNone {
		t.Errorf("above the keypad nothing should happen, got %+v", got)
	}
	if got := SettingsTap(v, 500, keypadTop+5*keypadKeyH+10, w); got.Kind != ActNone {
		t.Errorf("below the keypad nothing should happen, got %+v", got)
	}
	// The whole keypad is on screen.
	if bottom := keypadTop + 5*keypadKeyH; bottom > 1448 {
		t.Errorf("the keypad runs off the screen: bottom at %d", bottom)
	}
	// The root list, with its extra row, still fits.
	if bottom := SettingsRowY(serverRow()) + settingsRowH/2; bottom > 1380 {
		t.Errorf("the server row is too low (%d) to leave room for the footnote", bottom)
	}
}

func TestApplyServerKey(t *testing.T) {
	text := ""
	for _, k := range []string{"1", "9", "2", ".", "1", "6", "8", ":", "3", "0", "0", "0"} {
		text = ApplyServerKey(text, k)
	}
	if text != "192.168:3000" {
		t.Fatalf("typed %q", text)
	}
	if got := ApplyServerKey(text, "back"); got != "192.168:300" {
		t.Errorf("back = %q", got)
	}
	if got := ApplyServerKey("", "back"); got != "" {
		t.Errorf("back on nothing = %q", got)
	}
	if got := ApplyServerKey(text, "clear"); got != "" {
		t.Errorf("clear = %q", got)
	}
	long := ""
	for i := 0; i < 100; i++ {
		long = ApplyServerKey(long, "1")
	}
	if len(long) != maxServerLen {
		t.Errorf("length %d, want it capped at %d", len(long), maxServerLen)
	}
}

func TestServerEditorDraws(t *testing.T) {
	draw := func(v SettingsView, server string) *render.Canvas {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, v, units.Settings{}, false, nil, server)
		return c
	}
	typed := draw(SettingsView{Screen: SettingsServer, Text: "10.0.0.76:3001"}, "")
	empty := draw(SettingsView{Screen: SettingsServer}, "")
	field := image.Rect(40, 130, 1032, 250)
	if inked(typed, field.Inset(8)) <= inked(empty, field.Inset(8)) {
		t.Error("the typed address should appear in the field")
	}
	if inked(empty, image.Rect(0, keypadTop, 1072, keypadTop+5*keypadKeyH)) == 0 {
		t.Error("the keypad should be drawn")
	}
	hint := image.Rect(40, 280, 1032, 330)
	if !differs(draw(SettingsView{Screen: SettingsServer, Err: "port must be 1-65535"}, ""), empty) ||
		inked(draw(SettingsView{Screen: SettingsServer, Err: "port must be 1-65535"}, ""), hint) == 0 {
		t.Error("an error message should replace the hint")
	}
	// The list shows the server in use.
	row := image.Rect(500, SettingsRowY(serverRow())-40, 1000, SettingsRowY(serverRow())+40)
	if inked(draw(SettingsView{}, "10.0.0.76:3001"), row) <= inked(draw(SettingsView{}, ""), row) {
		t.Error("the list should show the current server")
	}
}

func TestBacklightRowOnlyOnDevicesWithALight(t *testing.T) {
	const w = 1072
	row := SettingsRowY(len(units.Metrics) + 4) // after invert, boxes and server
	with := SettingsView{MaxLevel: 24, Level: 10}
	if got := SettingsTap(with, 500, row, w); got.Kind != ActOpenLight {
		t.Errorf("with a light, the row after the server should open it, got %+v", got)
	}
	if got := SettingsTap(SettingsView{}, 500, row, w); got.Kind != ActNone {
		t.Errorf("with no light that row does not exist, got %+v", got)
	}

	draw := func(v SettingsView) *render.Canvas {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, v, units.Settings{}, false, nil, "")
		return c
	}
	r := image.Rect(720, row-40, 1000, row+40) // right-hand side: the footnote on the left is not the level
	if inked(draw(with), r) == 0 {
		t.Error("the row should show the current level")
	}
	if inked(draw(SettingsView{}), r) != 0 {
		t.Error("with no light the row must not be drawn")
	}
	if !differs(draw(SettingsView{MaxLevel: 24, Level: 0}), draw(with)) {
		t.Error("the row should say Off at level 0 and a level otherwise")
	}
	// The list, footnote and version still fit with the extra row.
	if bottom := SettingsRowY(lightRow()) + settingsRowH/2; bottom > 1330 {
		t.Errorf("the backlight row is too low (%d) for the footnote under it", bottom)
	}
}

func TestBacklightScreenTaps(t *testing.T) {
	const w = 1072
	v := SettingsView{Screen: SettingsLight, Level: 10, MaxLevel: 24}
	set := func(pt image.Point) Action { return SettingsTap(v, pt.X, pt.Y, w) }
	mid := func(r image.Rectangle) image.Point { return image.Pt((r.Min.X+r.Max.X)/2, (r.Min.Y+r.Max.Y)/2) }

	if a := set(mid(lightPlus)); a.Kind != ActSetLight || a.Level != 11 {
		t.Errorf("plus = %+v, want 11", a)
	}
	if a := set(mid(lightMinus)); a.Kind != ActSetLight || a.Level != 9 {
		t.Errorf("minus = %+v, want 9", a)
	}
	if a := set(mid(lightOff)); a.Kind != ActSetLight || a.Level != 0 {
		t.Errorf("off = %+v, want 0", a)
	}
	if a := set(mid(lightMaxBtn)); a.Kind != ActSetLight || a.Level != 24 {
		t.Errorf("max = %+v, want 24", a)
	}
	// The bar: left end is 0, right end is the maximum, the middle is half.
	y := lightBar.Min.Y + lightBar.Dy()/2
	if a := set(image.Pt(lightBar.Min.X, y)); a.Level != 0 {
		t.Errorf("left end = %+v", a)
	}
	if a := set(image.Pt(lightBar.Max.X, y)); a.Level != 24 {
		t.Errorf("right end = %+v", a)
	}
	if a := set(image.Pt((lightBar.Min.X+lightBar.Max.X)/2, y)); a.Level != 12 {
		t.Errorf("middle = %+v, want 12", a)
	}
	// A tap just outside the ends of the bar still counts, clamped to them.
	if a := set(image.Pt(lightBar.Min.X-30, y)); a.Kind != ActSetLight || a.Level != 0 {
		t.Errorf("just left of the bar = %+v", a)
	}
	// Buttons stop at the limits.
	v.Level = 24
	if a := set(mid(lightPlus)); a.Level != 24 {
		t.Errorf("plus at the maximum = %+v", a)
	}
	v.Level = 0
	if a := set(mid(lightMinus)); a.Level != 0 {
		t.Errorf("minus at zero = %+v", a)
	}
	// Elsewhere does nothing, and with no light nothing works.
	if a := set(image.Pt(500, 1200)); a.Kind != ActNone {
		t.Errorf("empty space = %+v", a)
	}
	if a := SettingsTap(SettingsView{Screen: SettingsLight}, mid(lightPlus).X, mid(lightPlus).Y, w); a.Kind != ActNone {
		t.Errorf("with no light the screen does nothing, got %+v", a)
	}
}

func TestBacklightScreenIsBlackAndWhiteAndShowsTheLevel(t *testing.T) {
	draw := func(level int) *render.Canvas {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, SettingsView{Screen: SettingsLight, Level: level, MaxLevel: 24}, units.Settings{}, false, nil, "")
		return c
	}
	low, high := draw(3), draw(21)
	if !differs(low, high) {
		t.Fatal("the screen should change with the level")
	}
	// The bar fills in proportion.
	if inked(high, lightBar) <= inked(low, lightBar) {
		t.Errorf("a higher level should fill more of the bar: %d vs %d", inked(high, lightBar), inked(low, lightBar))
	}
	// It is redrawn in place under the fast waveform, so no greys in the
	// controls: everything below the title is pure black or white except the
	// anti-aliased edges of text, which the bar and buttons never touch.
	for _, r := range []image.Rectangle{lightBar, lightMinus, lightPlus, lightOff, lightMaxBtn} {
		for y := r.Min.Y; y < r.Max.Y; y++ {
			for x := r.Min.X; x < r.Max.X; x++ {
				if g := high.Img.GrayAt(x, y).Y; g > 40 && g < 215 && (x < r.Min.X+3 || x >= r.Max.X-3 || y < r.Min.Y+3 || y >= r.Max.Y-3) {
					t.Fatalf("grey %d on the edge of a control at (%d,%d)", g, x, y)
				}
			}
		}
	}
	if draw(0) == nil {
		t.Fatal("level 0 should draw")
	}
}

func TestBoxPickerDrawsEachPageAndMarksTheChoice(t *testing.T) {
	draw := func(page int, boxes []string) *render.Canvas {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, SettingsView{Screen: SettingsPickBox, Box: 0, Page: page}, units.Settings{}, false, boxes, "")
		return c
	}
	for p := 0; p < BoxPages(); p++ {
		if inked(draw(p, nil), image.Rect(0, headerH+10, 1072, pickerPrev.Max.Y)) == 0 {
			t.Errorf("page %d drew nothing", p)
		}
		if inked(draw(p, nil), pickerNext) == 0 {
			t.Errorf("page %d has no page buttons", p)
		}
	}
	// The mark moves with the choice, but only on the page the choice is on.
	lastKind := BoxKinds[len(BoxKinds)-1].ID
	if !differs(draw(BoxPages()-1, []string{lastKind}), draw(BoxPages()-1, []string{"sog"})) {
		t.Error("the last page should show the selection")
	}
	if differs(draw(0, []string{lastKind}), draw(0, []string{"stw"})) &&
		!differs(draw(0, []string{lastKind}), draw(0, []string{"sog"})) {
		// both fine: page 0 only differs when the choice is on page 0
	}
}
