package settings

import (
	"path/filepath"
	"testing"
)

func TestGPSTimeoutValues(t *testing.T) {
	for _, n := range []int{0, 2, 3, 10, 60, 3600} {
		if !ValidNoGPS(n) {
			t.Errorf("%d should be valid", n)
		}
	}
	// One second would raise the alarm on a single late fix; and no negatives or day-long waits.
	for _, n := range []int{-1, 1, 3601, 86400} {
		if ValidNoGPS(n) {
			t.Errorf("%d should not be valid", n)
		}
	}
	if DefaultNoGPSSeconds != 10 {
		t.Errorf("the default is %d, want 10", DefaultNoGPSSeconds)
	}
}

func TestGPSTimeoutDefaultsSavesAndLoads(t *testing.T) {
	if got := (File{}).NoGPS(); got != DefaultNoGPSSeconds {
		t.Errorf("never chosen: %d, want the default %d", got, DefaultNoGPSSeconds)
	}
	zero, thirty := 0, 30
	if got := (File{NoGPSSeconds: &zero}).NoGPS(); got != 0 {
		t.Errorf("an explicit 0 (off) came back as %d", got)
	}

	path := filepath.Join(t.TempDir(), "settings.json")
	if err := Save(path, File{NoGPSSeconds: &thirty}); err != nil {
		t.Fatal(err)
	}
	f, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if f.NoGPSSeconds == nil || *f.NoGPSSeconds != 30 || f.NoGPS() != 30 {
		t.Errorf("loaded %+v", f.NoGPSSeconds)
	}
	// Off must survive a round trip as "off", not as "never chosen".
	if err := Save(path, File{NoGPSSeconds: &zero}); err != nil {
		t.Fatal(err)
	}
	if f, _ := Load(path); f.NoGPSSeconds == nil || f.NoGPS() != 0 {
		t.Errorf("off was lost: %v", f.NoGPSSeconds)
	}
	// The clone is independent.
	c := f.Clone() // f is still the 30-second file loaded first
	*c.NoGPSSeconds = 99
	if f.NoGPS() != 30 {
		t.Errorf("changing the clone changed the original, which now says %d", f.NoGPS())
	}
	// A file from before this setting existed has the default.
	old := filepath.Join(t.TempDir(), "old.json")
	Save(old, File{Invert: true})
	if f, _ := Load(old); f.NoGPS() != DefaultNoGPSSeconds {
		t.Errorf("an old file has %d, want the default", f.NoGPS())
	}
}
