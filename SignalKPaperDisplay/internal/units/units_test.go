package units

import (
	"path/filepath"
	"testing"
)

func TestConversions(t *testing.T) {
	s := Settings{Preset: PresetMetric}
	cases := []struct {
		metric, symbol string
		si             float64
		want           string
	}{
		{"sog", "kn", 1.0, "1.9"},
		{"sog", "km/h", 10, "36.0"},
		{"sog", "mph", 10, "22.4"},
		{"sog", "m/s", 3.14, "3.1"},
		{"depth", "ft", 10, "32.8"},
		{"depth", "fm", 10, "5.5"},
		{"range", "nm", 1852, "1.00"},
		{"range", "km", 1500, "1.50"},
	}
	for _, c := range cases {
		if err := s.SetUnit(c.metric, c.symbol); err != nil {
			t.Fatalf("SetUnit(%s,%s): %v", c.metric, c.symbol, err)
		}
		if got, _ := s.Format(c.metric, c.si); got != c.want {
			t.Errorf("%s %s: Format(%v) = %q, want %q", c.metric, c.symbol, c.si, got, c.want)
		}
	}
}

func TestTemperatureOffsets(t *testing.T) {
	c, _ := lookup(Temperature, "°C")
	f, _ := lookup(Temperature, "°F")
	if got := c.Format(293.15); got != "20.0" {
		t.Errorf("20°C = %q", got)
	}
	if got := f.Format(273.15); got != "32.0" {
		t.Errorf("0°C in °F = %q, want 32.0", got)
	}
}

func TestPresetAppliesToEverythingAndResetsOverrides(t *testing.T) {
	var s Settings
	if err := s.SetUnit("sog", "kn"); err != nil {
		t.Fatal(err)
	}
	if !s.IsOverridden("sog") {
		t.Error("sog should be overridden under metric (default km/h)")
	}

	if err := s.SetPreset(PresetImperial); err != nil {
		t.Fatal(err)
	}
	if got := s.UnitFor("sog").Symbol; got != "mph" {
		t.Errorf("after imperial preset sog = %s, want mph (override should be cleared)", got)
	}
	if got := s.UnitFor("depth").Symbol; got != "ft" {
		t.Errorf("imperial depth = %s, want ft", got)
	}
	if s.IsOverridden("sog") {
		t.Error("preset switch must clear overrides")
	}
}

func TestNauticalPreset(t *testing.T) {
	var s Settings
	if err := s.SetPreset(PresetNautical); err != nil {
		t.Fatal(err)
	}
	want := map[string]string{
		"sog": "kn", "stw": "kn", "aws": "kn", "tws": "kn", // every speed in knots
		"depth": "ft", "range": "nm",
	}
	for metric, sym := range want {
		if got := s.UnitFor(metric).Symbol; got != sym {
			t.Errorf("nautical %s = %s, want %s", metric, got, sym)
		}
	}
	// Knots are the preset's own default, so they aren't "set individually".
	if s.IsOverridden("sog") {
		t.Error("sog should not read as overridden under the nautical preset")
	}
	// Every preset must name a valid unit for every quantity, or UnitFor
	// would silently return a zero unit for some metric.
	for _, name := range Presets() {
		p := Settings{Preset: name}
		for _, m := range Metrics {
			if u := p.UnitFor(m.ID); u.Symbol == "" || u.Symbol == "?" {
				t.Errorf("preset %s has no valid unit for %s", name, m.ID)
			}
		}
	}
}

func TestOverrideLeavesOtherMetricsAlone(t *testing.T) {
	s := Settings{Preset: PresetImperial}
	if err := s.SetUnit("sog", "kn"); err != nil {
		t.Fatal(err)
	}
	if got := s.UnitFor("sog").Symbol; got != "kn" {
		t.Errorf("sog = %s, want kn", got)
	}
	if got := s.UnitFor("stw").Symbol; got != "mph" {
		t.Errorf("stw = %s, want mph (only sog was overridden)", got)
	}
	if !s.IsOverridden("sog") || s.IsOverridden("stw") {
		t.Error("only sog should be marked overridden")
	}
	// Choosing the preset's own default again drops the override.
	if err := s.SetUnit("sog", "mph"); err != nil {
		t.Fatal(err)
	}
	if s.IsOverridden("sog") {
		t.Error("setting back to the preset default should clear the override")
	}
}

func TestRejectsBadInput(t *testing.T) {
	var s Settings
	if err := s.SetUnit("sog", "fm"); err == nil {
		t.Error("fathoms are not a speed unit")
	}
	if err := s.SetUnit("nonsense", "kn"); err == nil {
		t.Error("unknown metric should fail")
	}
	if err := s.SetPreset("martian"); err == nil {
		t.Error("unknown preset should fail")
	}
	// A bad override already in a settings file falls back to the preset
	// rather than breaking the display.
	s = Settings{Preset: PresetMetric, Overrides: map[string]string{"sog": "parsecs"}}
	if got := s.UnitFor("sog").Symbol; got != "km/h" {
		t.Errorf("bad override should fall back to preset, got %s", got)
	}
}

func TestSaveLoadRoundTripAndMissingFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")

	def, err := Load(path)
	if err != nil || def.Preset != PresetMetric {
		t.Fatalf("missing file should give metric defaults, got %+v, %v", def, err)
	}

	s := Settings{Preset: PresetImperial}
	if err := s.SetUnit("depth", "fm"); err != nil {
		t.Fatal(err)
	}
	if err := Save(path, s); err != nil {
		t.Fatal(err)
	}
	got, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if got.Preset != PresetImperial || got.UnitFor("depth").Symbol != "fm" || got.UnitFor("sog").Symbol != "mph" {
		t.Errorf("round trip lost settings: %+v", got)
	}
}
