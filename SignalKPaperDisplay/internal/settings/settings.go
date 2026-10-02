// Package settings is everything the user can change, stored in one small
// JSON file next to the app. The unit choices live in internal/units; this
// adds the display options around them and owns reading and writing the file.
package settings

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"signalkpaperdisplay/internal/units"
)

// File is the contents of settings.json. The unit choices are embedded, so
// in the JSON they sit at the top level exactly as they did before there
// were any display options - a settings file written by an older version
// still loads unchanged.
type File struct {
	units.Settings
	Invert bool `json:"invert,omitempty"` // white on black instead of black on white
}

// Clone returns an independent copy (the unit overrides are a map).
func (f File) Clone() File {
	return File{Settings: f.Settings.Clone(), Invert: f.Invert}
}

// Load reads the file at path. A missing file is not an error - it just means
// defaults - so a fresh device works with no setup.
func Load(path string) (File, error) {
	b, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return File{Settings: units.Settings{Preset: units.PresetMetric}}, nil
	}
	if err != nil {
		return File{}, err
	}
	var f File
	if err := json.Unmarshal(b, &f); err != nil {
		return File{}, fmt.Errorf("%s: %w", path, err)
	}
	return f, nil
}

// Save writes the file atomically, so a power cut mid-write can't leave a
// half-written file that fails to parse on the next start.
func Save(path string, f File) error {
	b, err := json.MarshalIndent(f, "", "  ")
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".settings-*.json")
	if err != nil {
		return err
	}
	if _, err := tmp.Write(append(b, '\n')); err != nil {
		tmp.Close()
		os.Remove(tmp.Name())
		return err
	}
	if err := tmp.Close(); err != nil {
		os.Remove(tmp.Name())
		return err
	}
	return os.Rename(tmp.Name(), path)
}
