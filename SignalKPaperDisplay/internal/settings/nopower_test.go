package settings

import (
	"os"
	"path/filepath"
	"testing"
)

func TestNoPowerDefaultsToAnHourUntilChosen(t *testing.T) {
	var f File
	if f.NoPower() != 60 || DefaultNoPowerMinutes != 60 {
		t.Errorf("the default should be an hour, got %d", f.NoPower())
	}
	zero, five := 0, 5
	f.NoPowerMinutes = &zero
	if f.NoPower() != 0 {
		t.Error("an explicit zero means never, not the default")
	}
	f.NoPowerMinutes = &five
	if f.NoPower() != 5 {
		t.Errorf("a chosen 5 should be 5, got %d", f.NoPower())
	}
}

func TestValidNoPower(t *testing.T) {
	for _, ok := range []int{0, 1, 5, 60, 90, 480, MaxNoPowerMinutes} {
		if !ValidNoPower(ok) {
			t.Errorf("%d should be a valid timeout", ok)
		}
	}
	for _, bad := range []int{-1, -60, MaxNoPowerMinutes + 1, 1 << 30} {
		if ValidNoPower(bad) {
			t.Errorf("%d should not be", bad)
		}
	}
	if MaxNoPowerMinutes != 10080 {
		t.Errorf("the longest timeout is a week: %d", MaxNoPowerMinutes)
	}
}

func TestNoPowerIsSavedAndLoadedAndOnlyWhenChosen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "s.json")
	// Never chosen: not written, so a later change of the default still applies.
	if err := Save(path, File{}); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(path); string(b) != "" && contains(string(b), "noPower") {
		t.Errorf("an unchosen timeout was saved: %s", b)
	}
	if f, err := Load(path); err != nil || f.NoPowerMinutes != nil || f.NoPower() != 60 {
		t.Errorf("loaded %+v, %v", f, err)
	}
	// Chosen, including never (0).
	for _, m := range []int{0, 1, 90} {
		m := m
		if err := Save(path, File{NoPowerMinutes: &m}); err != nil {
			t.Fatal(err)
		}
		f, err := Load(path)
		if err != nil || f.NoPowerMinutes == nil || *f.NoPowerMinutes != m {
			t.Errorf("round trip of %d: %+v, %v", m, f.NoPowerMinutes, err)
		}
	}
	// A file from before there was a timeout still loads, as the default.
	if err := os.WriteFile(path, []byte(`{"preset":"nautical","invert":true}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if f, err := Load(path); err != nil || f.NoPower() != 60 || !f.Invert {
		t.Errorf("an old file: %+v, %v", f, err)
	}
}

func TestCloneKeepsTheTimeoutIndependent(t *testing.T) {
	n := 15
	a := File{NoPowerMinutes: &n}
	b := a.Clone()
	n = 99
	if b.NoPower() != 15 {
		t.Errorf("the copy shares the original's number: %d", b.NoPower())
	}
	if (File{}).Clone().NoPowerMinutes != nil {
		t.Error("cloning an unchosen timeout should leave it unchosen")
	}
}
