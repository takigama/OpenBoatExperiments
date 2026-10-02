package settings

import (
	"os"
	"path/filepath"
	"testing"

	"signalkpaperdisplay/internal/units"
)

func TestMissingFileGivesDefaults(t *testing.T) {
	f, err := Load(filepath.Join(t.TempDir(), "settings.json"))
	if err != nil {
		t.Fatal(err)
	}
	if f.Preset != units.PresetMetric || f.Invert {
		t.Errorf("defaults = %+v, want metric and not inverted", f)
	}
}

func TestSaveLoadRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	f := File{Settings: units.Settings{Preset: units.PresetImperial}, Invert: true}
	if err := f.SetUnit("depth", "fm"); err != nil {
		t.Fatal(err)
	}
	if err := Save(path, f); err != nil {
		t.Fatal(err)
	}
	got, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if got.Preset != units.PresetImperial || !got.Invert ||
		got.UnitFor("depth").Symbol != "fm" || got.UnitFor("sog").Symbol != "mph" {
		t.Errorf("round trip lost settings: %+v", got)
	}
}

// A settings.json written before display options existed has only the unit
// fields. It must keep loading, with the new option off.
func TestAFileFromBeforeDisplayOptionsStillLoads(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	old := `{"preset": "nautical", "overrides": {"depth": "m"}}`
	if err := os.WriteFile(path, []byte(old), 0o644); err != nil {
		t.Fatal(err)
	}
	f, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if f.Preset != units.PresetNautical || f.UnitFor("depth").Symbol != "m" || f.UnitFor("sog").Symbol != "kn" {
		t.Errorf("an old file loaded as %+v", f)
	}
	if f.Invert {
		t.Error("the new option must default to off for an old file")
	}
}

func TestUnitFieldsStayAtTheTopLevelOfTheJSON(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	if err := Save(path, File{Settings: units.Settings{Preset: units.PresetNautical}, Invert: true}); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(path)
	s := string(b)
	for _, want := range []string{`"preset": "nautical"`, `"invert": true`} {
		if !contains(s, want) {
			t.Errorf("settings.json is missing %s:\n%s", want, s)
		}
	}
	if contains(s, `"Settings"`) {
		t.Errorf("the units must be flattened, not nested under a key:\n%s", s)
	}
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

func TestCloneIsIndependent(t *testing.T) {
	a := File{Settings: units.Settings{Preset: units.PresetMetric}}
	a.SetUnit("sog", "kn")
	b := a.Clone()
	b.SetUnit("sog", "mph")
	b.Invert = true
	if a.UnitFor("sog").Symbol != "kn" || a.Invert {
		t.Errorf("editing a clone changed the original: %+v", a)
	}
}
