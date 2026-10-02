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
	if got := SettingsTap(root, 500, rowY(len(units.Metrics)+3), 1072); got.Kind != ActNone {
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
		Settings(c, SettingsView{}, units.Settings{}, true, nil)
		return c
	}(), func() *render.Canvas {
		c, _ := render.NewCanvas(1072, 1448)
		Settings(c, SettingsView{}, units.Settings{}, false, nil)
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
		Settings(c, v, u, false, nil)
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
		Settings(c, SettingsView{}, units.Settings{}, false, nil)
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

	// The picker is two columns, filled row by row.
	pick := SettingsView{Screen: SettingsPickBox, Box: 3}
	for i, k := range BoxKinds {
		x := 200
		if i%2 == 1 {
			x = 800
		}
		got := SettingsTap(pick, x, SettingsRowY(i/2), w)
		if got.Kind != ActSetBox || got.Value != k.ID || got.Box != 3 {
			t.Errorf("kind %d (%s) = %+v", i, k.ID, got)
		}
	}
	if len(BoxKinds)%2 == 1 {
		if got := SettingsTap(pick, 800, SettingsRowY(len(BoxKinds)/2), w); got.Kind != ActNone {
			t.Errorf("the empty slot after the last kind should do nothing, got %+v", got)
		}
	}
	// Everything fits on screen with room for the back chevron.
	if last := SettingsRowY((len(BoxKinds)-1)/2) + settingsRowH/2; last > 1448 {
		t.Errorf("the picker runs off the bottom of the screen at y=%d", last)
	}

	if ParentScreen(SettingsPickBox) != SettingsBoxes || ParentScreen(SettingsBoxes) != SettingsRoot ||
		ParentScreen(SettingsPickUnit) != SettingsRoot {
		t.Error("back should go pickers -> their list -> root")
	}
}

func TestSettingsBoxScreensDraw(t *testing.T) {
	for _, v := range []SettingsView{{Screen: SettingsBoxes}, {Screen: SettingsPickBox, Box: 5}, {Screen: SettingsPickBox, Box: 99}} {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Settings(c, v, units.Settings{}, false, []string{"stw"})
		if inked(c, image.Rect(0, headerH+10, 1072, 1448)) == 0 {
			t.Errorf("%+v drew nothing", v)
		}
	}
	// The picker marks the current choice.
	draw := func(boxes []string) *render.Canvas {
		c, _ := render.NewCanvas(1072, 1448)
		Settings(c, SettingsView{Screen: SettingsPickBox, Box: 0}, units.Settings{}, false, boxes)
		return c
	}
	if !differs(draw([]string{"sog"}), draw([]string{"stw"})) {
		t.Error("the picker should show which kind is selected")
	}
}
