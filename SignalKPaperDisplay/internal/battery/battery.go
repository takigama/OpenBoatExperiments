// Package battery reads the device's own battery: how full it is and whether
// it is on external power. On the Kindle that is the Linux power_supply class
// in sysfs, or failing that the power daemon's lipc properties.
package battery

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Status is the battery at a moment.
type Status struct {
	Percent  int  // 0..100, or -1 if unknown
	Plugged  bool // on external power (charging, or full and still plugged in)
	Charging bool // actually taking charge now
}

// Reader reports the battery.
type Reader interface {
	Read() (Status, error)
}

// DefaultSysfs is where Linux lists its power supplies.
const DefaultSysfs = "/sys/class/power_supply"

// Detect returns a reader for this device: the sysfs power supplies if there
// is a battery among them, else the Kindle's power daemon, else an error.
func Detect() (Reader, error) {
	if r, err := NewSysfs(DefaultSysfs); err == nil {
		return r, nil
	}
	if _, err := exec.LookPath("lipc-get-prop"); err == nil {
		l := &Lipc{}
		if _, err := l.Read(); err == nil {
			return l, nil
		}
	}
	return nil, fmt.Errorf("no battery found")
}

func readString(path string) (string, bool) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", false
	}
	return strings.TrimSpace(string(b)), true
}

func readInt(path string) (int, bool) {
	s, ok := readString(path)
	if !ok {
		return 0, false
	}
	n, err := strconv.Atoi(s)
	return n, err == nil
}

// Sysfs reads a power_supply directory: each entry has a "type" file saying
// Battery, Mains, USB and so on.
type Sysfs struct {
	root string
	bat  string // the battery's directory
}

// NewSysfs finds the battery under root.
func NewSysfs(root string) (*Sysfs, error) {
	entries, _ := filepath.Glob(filepath.Join(root, "*"))
	for _, d := range entries {
		if t, ok := readString(filepath.Join(d, "type")); ok && strings.EqualFold(t, "Battery") {
			if _, ok := readInt(filepath.Join(d, "capacity")); ok {
				return &Sysfs{root: root, bat: d}, nil
			}
		}
	}
	return nil, fmt.Errorf("no battery under %s", root)
}

func (s *Sysfs) Read() (Status, error) {
	pct, ok := readInt(filepath.Join(s.bat, "capacity"))
	if !ok {
		return Status{Percent: -1}, fmt.Errorf("cannot read the battery level")
	}
	st := Status{Percent: max(0, min(pct, 100))}

	state, _ := readString(filepath.Join(s.bat, "status"))
	switch strings.ToLower(state) {
	case "charging":
		st.Charging, st.Plugged = true, true
	case "full":
		st.Plugged = true // full only happens on external power
	}
	// Any other supply that is online (a charger, USB) means plugged in; some
	// chargers also say so with a "charging" file of their own.
	others, _ := filepath.Glob(filepath.Join(s.root, "*"))
	for _, d := range others {
		if d == s.bat {
			continue
		}
		if n, ok := readInt(filepath.Join(d, "online")); ok && n == 1 {
			st.Plugged = true
		}
		if n, ok := readInt(filepath.Join(d, "charging")); ok && n == 1 {
			st.Plugged, st.Charging = true, true
		}
	}
	return st, nil
}

// Lipc reads the Kindle power daemon's properties.
type Lipc struct {
	run func(ctx context.Context, name string, args ...string) (string, error)
}

func (l *Lipc) get(prop string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var out string
	var err error
	if l.run != nil {
		out, err = l.run(ctx, "lipc-get-prop", "com.lab126.powerd", prop)
	} else {
		var b []byte
		b, err = exec.CommandContext(ctx, "lipc-get-prop", "com.lab126.powerd", prop).CombinedOutput()
		out = string(b)
	}
	return strings.TrimSpace(out), err
}

func (l *Lipc) Read() (Status, error) {
	lvl, err := l.get("battLevel")
	if err != nil {
		return Status{Percent: -1}, fmt.Errorf("lipc-get-prop battLevel: %w", err)
	}
	pct, err := strconv.Atoi(lvl)
	if err != nil {
		return Status{Percent: -1}, fmt.Errorf("lipc-get-prop battLevel gave %q", lvl)
	}
	st := Status{Percent: max(0, min(pct, 100))}
	if c, err := l.get("isCharging"); err == nil && c == "1" {
		st.Charging, st.Plugged = true, true
	}
	return st, nil
}

// Fake is a reader with a fixed answer, for previews on a PC and for tests.
type Fake struct {
	S   Status
	Err error
}

func (f *Fake) Read() (Status, error) { return f.S, f.Err }
