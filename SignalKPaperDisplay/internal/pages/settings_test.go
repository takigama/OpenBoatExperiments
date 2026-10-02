package pages

import (
	"testing"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/units"
)

// rowY is the middle of list row i.
func rowY(i int) int { return settingsTop + i*settingsRowH + settingsRowH/2 }

func TestSettingsTapRoot(t *testing.T) {
	root := SettingsView{Screen: SettingsRoot}

	if got := SettingsTap(root, 500, rowY(0)); got.Kind != ActOpenPresetPicker {
		t.Errorf("row 0 = %+v, want the preset picker", got)
	}
	// Every metric row opens its own unit picker, in the order of units.Metrics.
	for i, m := range units.Metrics {
		got := SettingsTap(root, 500, rowY(i+1))
		if got.Kind != ActOpenUnitPicker || got.Metric != m.ID {
			t.Errorf("row %d = %+v, want unit picker for %s", i+1, got, m.ID)
		}
	}
	if got := SettingsTap(root, 500, rowY(len(units.Metrics)+3)); got.Kind != ActNone {
		t.Errorf("a tap below the list = %+v, want nothing", got)
	}
	if got := SettingsTap(root, 500, headerH+5); got.Kind != ActNone {
		t.Errorf("a tap in the header gap = %+v, want nothing", got)
	}
}

func TestSettingsTapPickers(t *testing.T) {
	p := units.Presets()
	if got := SettingsTap(SettingsView{Screen: SettingsPickPreset}, 500, rowY(1)); got.Kind != ActSetPreset || got.Value != p[1] {
		t.Errorf("preset row 1 = %+v, want preset %s", got, p[1])
	}

	v := SettingsView{Screen: SettingsPickUnit, Metric: "depth"}
	opts := units.Units(units.Depth)
	for i, o := range opts {
		got := SettingsTap(v, 500, rowY(i))
		if got.Kind != ActSetUnit || got.Metric != "depth" || got.Value != o.Symbol {
			t.Errorf("depth row %d = %+v, want %s", i, got, o.Symbol)
		}
	}
	if got := SettingsTap(v, 500, rowY(len(opts))); got.Kind != ActNone {
		t.Errorf("a tap past the last choice = %+v, want nothing", got)
	}
}

func TestCogAndBackShareTheSameTapArea(t *testing.T) {
	x, y := CogRect.Min.X+5, CogRect.Min.Y+5
	for _, s := range []SettingsScreen{SettingsRoot, SettingsPickPreset, SettingsPickUnit} {
		if got := SettingsTap(SettingsView{Screen: s, Metric: "sog"}, x, y); got.Kind != ActBack {
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
		Settings(c, v, u)
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
