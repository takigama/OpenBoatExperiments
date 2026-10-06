// Package settings is everything the user can change, stored in one small
// JSON file next to the app. The unit choices live in internal/units; this
// adds the display options around them and owns reading and writing the file.
package settings

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
	_ "time/tzdata" // the zone database, built in: a Kindle has none we can rely on

	"signalkpaperdisplay/internal/units"
)

// File is the contents of settings.json. The unit choices are embedded, so
// in the JSON they sit at the top level exactly as they did before there
// were any display options - a settings file written by an older version
// still loads unchanged.
type File struct {
	units.Settings
	Invert bool `json:"invert,omitempty"` // white on black instead of black on white
	// Boxes is what each of the Nav page's six boxes shows, as page box-kind
	// IDs, left to right then top to bottom. Missing or unknown entries fall
	// back to the defaults, so a short or old file is fine.
	Boxes []string `json:"boxes,omitempty"`
	// Server is the SignalK server as "host:port". When set it wins over the
	// -signalk flag (and so the launcher's SIGNALK_HOST), so a change made on
	// the device sticks.
	Server string `json:"server,omitempty"`
	// Brightness is the front light level the user chose, applied when the
	// app starts. Nil means never set: the light is left as the device has it.
	Brightness *int `json:"brightness,omitempty"`
	// NoPowerMinutes is how long the device may be off external power before the
	// display goes to its NO POWER screen and stops updating to save the battery.
	// Nil means never chosen: DefaultNoPowerMinutes. Zero means never.
	NoPowerMinutes *int `json:"noPowerMinutes,omitempty"`
	// Timezone is the IANA name the clock is shown in ("Australia/Sydney"). Empty
	// means the device's own zone, which on a Kindle is usually not the boat's.
	Timezone string `json:"timezone,omitempty"`
	// IdleEnabled turns on idle mode: the dashboard goes to its IDLE screen while the
	// SignalK switch at IdlePath is off. Empty IdlePath means DefaultIdlePath.
	IdleEnabled bool   `json:"idleEnabled,omitempty"`
	IdlePath    string `json:"idlePath,omitempty"`
	// NoGPSSeconds is how long our own GPS position may go without an update before
	// the header says NO DATA. Nil means never chosen: DefaultNoGPSSeconds. Zero turns
	// the check off.
	NoGPSSeconds *int `json:"noGpsSeconds,omitempty"`
}

// DefaultNoGPSSeconds is the GPS timeout until it is changed. A GPS fix comes about
// once a second, so this is about ten missed fixes.
const DefaultNoGPSSeconds = 10

// The shortest and longest GPS timeout that can be set (zero, off, is also allowed).
// Under two seconds a single late fix would raise the alarm.
const (
	MinNoGPSSeconds = 2
	MaxNoGPSSeconds = 3600
)

// ValidNoGPS reports whether seconds is a GPS timeout that can be set: zero (off) or
// from MinNoGPSSeconds to MaxNoGPSSeconds.
func ValidNoGPS(seconds int) bool {
	return seconds == 0 || (seconds >= MinNoGPSSeconds && seconds <= MaxNoGPSSeconds)
}

// NoGPS is the GPS timeout in force, in seconds: the chosen one, else the default.
// Zero means off.
func (f File) NoGPS() int {
	if f.NoGPSSeconds == nil {
		return DefaultNoGPSSeconds
	}
	return *f.NoGPSSeconds
}

// DefaultIdlePath is the switch idle mode watches until another is chosen: the first
// channel of the first switch bank, in SignalK's form
// electrical.switches.bank.<bank>.<channel>.state (channels are numbered from 1).
const DefaultIdlePath = "electrical.switches.bank.0.1.state"

// IdleSwitchPath is the SignalK path idle mode watches: the chosen one, else the
// default.
func (f File) IdleSwitchPath() string {
	if f.IdlePath == "" {
		return DefaultIdlePath
	}
	return f.IdlePath
}

// ParseTimezone turns a zone name into a location. "" (and "Local") is the device's
// own zone, returned as nil. The zone database is built into the program, so this
// does not depend on the device having one.
func ParseTimezone(name string) (*time.Location, error) {
	name = strings.TrimSpace(name)
	if name == "" || name == "Local" {
		return nil, nil
	}
	if len(name) > 64 {
		return nil, fmt.Errorf("a time zone name is at most 64 characters")
	}
	for _, r := range name {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '/', r == '_', r == '-', r == '+':
		default:
			return nil, fmt.Errorf("%q is not a time zone name like Australia/Sydney", name)
		}
	}
	loc, err := time.LoadLocation(name)
	if err != nil {
		return nil, fmt.Errorf("%q is not a time zone name like Australia/Sydney", name)
	}
	return loc, nil
}

// DefaultNoPowerMinutes is the no-power timeout until it is changed: an hour.
const DefaultNoPowerMinutes = 60

// MaxNoPowerMinutes is the longest timeout that can be set: a week.
const MaxNoPowerMinutes = 7 * 24 * 60

// ValidNoPower reports whether minutes is a timeout that can be set: zero (never)
// or from one minute to MaxNoPowerMinutes.
func ValidNoPower(minutes int) bool {
	return minutes == 0 || (minutes >= 1 && minutes <= MaxNoPowerMinutes)
}

// NoPower is the timeout in force, in minutes: the chosen one, else the default.
// Zero means never.
func (f File) NoPower() int {
	if f.NoPowerMinutes == nil {
		return DefaultNoPowerMinutes
	}
	return *f.NoPowerMinutes
}

// Clone returns an independent copy (the unit overrides are a map).
func (f File) Clone() File {
	c := File{Settings: f.Settings.Clone(), Invert: f.Invert, Boxes: append([]string(nil), f.Boxes...), Server: f.Server, Brightness: f.cloneBrightness(),
		Timezone: f.Timezone, IdleEnabled: f.IdleEnabled, IdlePath: f.IdlePath}
	if f.NoPowerMinutes != nil {
		n := *f.NoPowerMinutes
		c.NoPowerMinutes = &n
	}
	if f.NoGPSSeconds != nil {
		n := *f.NoGPSSeconds
		c.NoGPSSeconds = &n
	}
	return c
}

func (f File) cloneBrightness() *int {
	if f.Brightness == nil {
		return nil
	}
	v := *f.Brightness
	return &v
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

// DefaultPort is SignalK's usual port, used when an address has none.
const DefaultPort = 3000

// NormalizeServer checks a typed server address and returns it as
// "host:port". The host is an IPv4 address or a hostname; a missing port
// becomes DefaultPort.
func NormalizeServer(s string) (string, error) {
	s = strings.TrimSpace(s)
	host, port := s, strconv.Itoa(DefaultPort)
	if i := strings.LastIndex(s, ":"); i >= 0 {
		host, port = s[:i], s[i+1:]
	}
	if host == "" {
		return "", fmt.Errorf("no address")
	}
	if n, err := strconv.Atoi(port); err != nil || n < 1 || n > 65535 {
		return "", fmt.Errorf("port must be 1-65535")
	}
	if !validHost(host) {
		return "", fmt.Errorf("not a valid address")
	}
	return host + ":" + port, nil
}

func validHost(h string) bool {
	labels := strings.Split(h, ".")
	numeric := true
	for _, l := range labels {
		if l == "" || len(l) > 63 {
			return false
		}
		for _, r := range l {
			switch {
			case r >= '0' && r <= '9':
			case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r == '-':
				numeric = false
			default:
				return false
			}
		}
	}
	if numeric { // looks like an IPv4 address, so it has to be one
		if len(labels) != 4 {
			return false
		}
		for _, l := range labels {
			if n, _ := strconv.Atoi(l); n > 255 || len(l) > 3 {
				return false
			}
		}
	}
	return true
}
