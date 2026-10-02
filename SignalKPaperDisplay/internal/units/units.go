// Package units converts SignalK's SI values into whatever the user wants
// to see, per metric. A Settings value holds a preset (metric/imperial) plus
// optional per-metric overrides, so "everything imperial, except speed in
// knots" is expressible - and pages only ever ask Settings to format a
// metric, never hard-code a unit.
package units

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
)

// Quantity is a kind of measurement; each has its own set of units.
type Quantity string

const (
	Speed       Quantity = "speed"       // SI: m/s
	Depth       Quantity = "depth"       // SI: m
	Distance    Quantity = "distance"    // SI: m
	Temperature Quantity = "temperature" // SI: K
	Pressure    Quantity = "pressure"    // SI: Pa
)

// Unit converts from the SI base: value = si*Scale + Offset.
type Unit struct {
	Symbol   string
	Scale    float64
	Offset   float64
	Decimals int
}

func (u Unit) Convert(si float64) float64 { return si*u.Scale + u.Offset }

func (u Unit) Format(si float64) string {
	return strconv.FormatFloat(u.Convert(si), 'f', u.Decimals, 64)
}

var table = map[Quantity][]Unit{
	Speed: {
		{"kn", 1 / 0.514444, 0, 1},
		{"km/h", 3.6, 0, 1},
		{"mph", 2.236936, 0, 1},
		{"m/s", 1, 0, 1},
	},
	Depth: {
		{"m", 1, 0, 1},
		{"ft", 3.280840, 0, 1},
		{"fm", 0.546807, 0, 1}, // fathoms
	},
	Distance: {
		{"nm", 1 / 1852.0, 0, 2},
		{"km", 0.001, 0, 2},
		{"mi", 1 / 1609.344, 0, 2},
		{"m", 1, 0, 0},
		{"ft", 3.280840, 0, 0},
	},
	Temperature: {
		{"°C", 1, -273.15, 1},
		{"°F", 1.8, -459.67, 1},
		{"K", 1, 0, 1},
	},
	Pressure: {
		{"hPa", 0.01, 0, 0},
		{"inHg", 0.0002953, 0, 2},
		{"mmHg", 0.00750062, 0, 0},
		{"bar", 0.00001, 0, 3},
		{"psi", 0.000145038, 0, 1},
		{"kPa", 0.001, 0, 1},
	},
}

// Units lists the choices for a quantity, default-first order not implied.
func Units(q Quantity) []Unit { return table[q] }

func lookup(q Quantity, symbol string) (Unit, bool) {
	for _, u := range table[q] {
		if u.Symbol == symbol {
			return u, true
		}
	}
	return Unit{}, false
}

// Metric is one displayed value that has its own unit setting.
type Metric struct {
	ID    string
	Label string
	Qty   Quantity
}

// Metrics is every value that gets its own unit setting, in the order the
// settings dialog lists them. Add a row here and it appears there.
var Metrics = []Metric{
	{"sog", "Speed over ground", Speed},
	{"stw", "Speed through water", Speed},
	{"aws", "Apparent wind speed", Speed},
	{"tws", "True wind speed", Speed},
	{"depth", "Depth", Depth},
	{"range", "AIS target range", Distance},
}

func metricByID(id string) (Metric, bool) {
	for _, m := range Metrics {
		if m.ID == id {
			return m, true
		}
	}
	return Metric{}, false
}

// Preset names and the unit each quantity defaults to under that preset.
// These are the defaults only - any metric can be overridden individually.
const (
	PresetMetric   = "metric"
	PresetImperial = "imperial"
)

var presets = map[string]map[Quantity]string{
	PresetMetric: {
		Speed: "km/h", Depth: "m", Distance: "km", Temperature: "°C", Pressure: "hPa",
	},
	PresetImperial: {
		Speed: "mph", Depth: "ft", Distance: "mi", Temperature: "°F", Pressure: "inHg",
	},
}

// Presets lists the preset names in display order.
func Presets() []string { return []string{PresetMetric, PresetImperial} }

// Settings is the user's unit choices. The zero value is the metric preset
// with no overrides.
type Settings struct {
	Preset    string            `json:"preset"`
	Overrides map[string]string `json:"overrides,omitempty"` // metric ID -> unit symbol
}

// Clone returns an independent copy. Settings holds a map, so a plain
// assignment shares it - and the touch handler edits settings while the
// renderer reads them.
func (s Settings) Clone() Settings {
	c := Settings{Preset: s.Preset}
	if len(s.Overrides) > 0 {
		c.Overrides = make(map[string]string, len(s.Overrides))
		for k, v := range s.Overrides {
			c.Overrides[k] = v
		}
	}
	return c
}

func (s Settings) preset() map[Quantity]string {
	if p, ok := presets[s.Preset]; ok {
		return p
	}
	return presets[PresetMetric]
}

// UnitFor is the unit currently in effect for a metric: its override if
// valid, otherwise the preset's default for that quantity.
func (s Settings) UnitFor(metricID string) Unit {
	m, ok := metricByID(metricID)
	if !ok {
		return Unit{Symbol: "?", Scale: 1}
	}
	if sym, ok := s.Overrides[metricID]; ok {
		if u, ok := lookup(m.Qty, sym); ok {
			return u
		}
	}
	u, _ := lookup(m.Qty, s.preset()[m.Qty])
	return u
}

// Format renders an SI value in the metric's chosen unit.
func (s Settings) Format(metricID string, si float64) (value, unit string) {
	u := s.UnitFor(metricID)
	return u.Format(si), u.Symbol
}

// IsOverridden reports whether a metric differs from its preset default,
// so the dialog can mark it.
func (s Settings) IsOverridden(metricID string) bool {
	m, ok := metricByID(metricID)
	if !ok {
		return false
	}
	return s.UnitFor(metricID).Symbol != s.preset()[m.Qty]
}

// SetPreset switches every metric to a preset's defaults, discarding any
// overrides - that's what "set everything to imperial" means.
func (s *Settings) SetPreset(name string) error {
	if _, ok := presets[name]; !ok {
		return fmt.Errorf("unknown preset %q", name)
	}
	s.Preset = name
	s.Overrides = nil
	return nil
}

// SetUnit overrides one metric's unit. Setting it back to the preset
// default drops the override, so IsOverridden stays accurate.
func (s *Settings) SetUnit(metricID, symbol string) error {
	m, ok := metricByID(metricID)
	if !ok {
		return fmt.Errorf("unknown metric %q", metricID)
	}
	if _, ok := lookup(m.Qty, symbol); !ok {
		return fmt.Errorf("%q is not a valid unit for %s", symbol, m.Label)
	}
	if symbol == s.preset()[m.Qty] {
		delete(s.Overrides, metricID)
		return nil
	}
	if s.Overrides == nil {
		s.Overrides = map[string]string{}
	}
	s.Overrides[metricID] = symbol
	return nil
}

// Load reads settings from path. A missing file is not an error - it just
// means defaults - so a fresh device works with no setup.
func Load(path string) (Settings, error) {
	b, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return Settings{Preset: PresetMetric}, nil
	}
	if err != nil {
		return Settings{}, err
	}
	var s Settings
	if err := json.Unmarshal(b, &s); err != nil {
		return Settings{}, fmt.Errorf("%s: %w", path, err)
	}
	return s, nil
}

// Save writes settings atomically, so a power cut mid-write can't leave a
// half-written file that fails to parse on the next start.
func Save(path string, s Settings) error {
	b, err := json.MarshalIndent(s, "", "  ")
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
