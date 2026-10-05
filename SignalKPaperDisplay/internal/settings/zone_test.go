package settings

import (
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestParseTimezone(t *testing.T) {
	// The device's own zone is nil, however it is spelled.
	for _, own := range []string{"", "  ", "Local"} {
		if loc, err := ParseTimezone(own); loc != nil || err != nil {
			t.Errorf("%q = (%v, %v), want the device's own zone (nil, nil)", own, loc, err)
		}
	}

	// A real zone, found without the device having a zone database: Sydney is on
	// +11 in January and +10 in July.
	loc, err := ParseTimezone("Australia/Sydney")
	if err != nil || loc == nil {
		t.Fatalf("Australia/Sydney: %v, %v", loc, err)
	}
	_, summer := time.Date(2026, 1, 15, 12, 0, 0, 0, time.UTC).In(loc).Zone()
	_, winter := time.Date(2026, 7, 15, 12, 0, 0, 0, time.UTC).In(loc).Zone()
	if summer != 11*3600 || winter != 10*3600 {
		t.Errorf("Sydney is UTC%+d in January and UTC%+d in July, want +11 and +10", summer/3600, winter/3600)
	}
	if loc, err := ParseTimezone(" Pacific/Auckland "); err != nil || loc == nil {
		t.Errorf("a name with spaces round it should be trimmed: %v, %v", loc, err)
	}
	if loc, err := ParseTimezone("UTC"); err != nil || loc == nil {
		t.Errorf("UTC: %v, %v", loc, err)
	}

	for _, bad := range []string{
		"Mars/Olympus", "Australia/", "/etc/passwd", "../../etc/passwd", "Australia/Sy\ndney", "Australia/Syd ney",
		"a;b", "$(reboot)", strings.Repeat("A", 65),
	} {
		if loc, err := ParseTimezone(bad); err == nil {
			t.Errorf("%q was accepted as %v", bad, loc)
		}
	}
}

func TestTimezoneAndIdleAreSavedAndLoaded(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	if err := Save(path, File{Timezone: "Australia/Sydney", IdleEnabled: true, IdlePath: "electrical.switches.nav.state"}); err != nil {
		t.Fatal(err)
	}
	got, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if got.Timezone != "Australia/Sydney" || !got.IdleEnabled || got.IdlePath != "electrical.switches.nav.state" {
		t.Errorf("loaded %+v", got)
	}
	c := got.Clone()
	if c.Timezone != got.Timezone || c.IdleEnabled != got.IdleEnabled || c.IdlePath != got.IdlePath {
		t.Errorf("the clone lost something: %+v", c)
	}

	// A file written before these existed loads with them off.
	old := filepath.Join(t.TempDir(), "old.json")
	if err := Save(old, File{Invert: true}); err != nil {
		t.Fatal(err)
	}
	if f, _ := Load(old); f.Timezone != "" || f.IdleEnabled || f.IdlePath != "" || !f.Invert {
		t.Errorf("an old file loaded as %+v", f)
	}
}

func TestIdleSwitchPathDefaults(t *testing.T) {
	if got := (File{}).IdleSwitchPath(); got != DefaultIdlePath {
		t.Errorf("default = %q", got)
	}
	if got := (File{IdlePath: "electrical.switches.nav.state"}).IdleSwitchPath(); got != "electrical.switches.nav.state" {
		t.Errorf("chosen = %q", got)
	}
}
