package frontlight

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// fakeBacklight makes a directory like /sys/class/backlight/<name>.
func fakeBacklight(t *testing.T, name string, max, level string) string {
	t.Helper()
	root := t.TempDir()
	d := filepath.Join(root, name)
	if err := os.MkdirAll(d, 0o755); err != nil {
		t.Fatal(err)
	}
	for f, v := range map[string]string{"max_brightness": max + "\n", "brightness": level + "\n"} {
		if err := os.WriteFile(filepath.Join(d, f), []byte(v), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return filepath.Join(root, "*")
}

func TestSysfsReadsAndWrites(t *testing.T) {
	glob := fakeBacklight(t, "max77696-bl", "24", "5")
	l, err := NewSysfs(glob)
	if err != nil {
		t.Fatal(err)
	}
	if l.Max() != 24 {
		t.Errorf("max = %d, want 24", l.Max())
	}
	if n, err := l.Level(); err != nil || n != 5 {
		t.Errorf("level = %d, %v; want 5", n, err)
	}
	if err := l.Set(17); err != nil {
		t.Fatal(err)
	}
	if n, _ := l.Level(); n != 17 {
		t.Errorf("after Set(17) the level is %d", n)
	}
	// Out-of-range requests are clamped, never written raw.
	l.Set(99)
	if n, _ := l.Level(); n != 24 {
		t.Errorf("Set(99) left %d, want the maximum 24", n)
	}
	l.Set(-3)
	if n, _ := l.Level(); n != 0 {
		t.Errorf("Set(-3) left %d, want 0", n)
	}
}

func TestSysfsSkipsDirectoriesThatAreNotBacklights(t *testing.T) {
	root := t.TempDir()
	os.MkdirAll(filepath.Join(root, "a-no-files"), 0o755)
	bad := filepath.Join(root, "b-bad-max")
	os.MkdirAll(bad, 0o755)
	os.WriteFile(filepath.Join(bad, "max_brightness"), []byte("zero"), 0o644)
	good := filepath.Join(root, "c-good")
	os.MkdirAll(good, 0o755)
	os.WriteFile(filepath.Join(good, "max_brightness"), []byte("10"), 0o644)
	os.WriteFile(filepath.Join(good, "brightness"), []byte("3"), 0o644)

	l, err := NewSysfs(filepath.Join(root, "*"))
	if err != nil || l.Max() != 10 {
		t.Fatalf("got %+v, %v; want the good directory", l, err)
	}
	if _, err := NewSysfs(filepath.Join(root, "nothing-*")); err == nil {
		t.Error("a glob that matches nothing is an error")
	}
}

func TestLipcLight(t *testing.T) {
	var calls [][]string
	level := "7"
	l := &Lipc{Prop: "com.lab126.powerd flIntensity", MaxLevel: 24,
		run: func(_ context.Context, name string, args ...string) (string, error) {
			calls = append(calls, append([]string{name}, args...))
			return level + "\n", nil
		}}
	if n, err := l.Level(); err != nil || n != 7 {
		t.Errorf("level = %d, %v", n, err)
	}
	if err := l.Set(30); err != nil { // clamped to 24
		t.Fatal(err)
	}
	want := [][]string{
		{"lipc-get-prop", "com.lab126.powerd", "flIntensity"},
		{"lipc-set-prop", "-i", "com.lab126.powerd", "flIntensity", "24"},
	}
	if !reflect.DeepEqual(calls, want) {
		t.Errorf("calls = %v, want %v", calls, want)
	}
}

func TestLipcErrorsAreReported(t *testing.T) {
	l := &Lipc{Prop: "a b", MaxLevel: 5, run: func(context.Context, string, ...string) (string, error) {
		return "no such property", errors.New("exit status 1")
	}}
	if err := l.Set(1); err == nil {
		t.Error("a failing lipc-set-prop must be an error")
	}
	if _, err := l.Level(); err == nil {
		t.Error("a failing lipc-get-prop must be an error")
	}
	l.run = func(context.Context, string, ...string) (string, error) { return "garbage", nil }
	if _, err := l.Level(); err == nil {
		t.Error("a non-numeric answer must be an error")
	}
}

func TestDetectPrefersSysfsAndFailsCleanly(t *testing.T) {
	glob := fakeBacklight(t, "bl", "12", "0")
	l, err := Detect(Config{Sysfs: glob, Lipc: "a b", LipcMax: 24})
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := l.(*Sysfs); !ok || l.Max() != 12 {
		t.Errorf("got %T max %d, want the sysfs light", l, l.Max())
	}
	if _, err := Detect(Config{Sysfs: "/nonexistent/*"}); err == nil {
		t.Error("with no light at all Detect should say so")
	}
	if _, err := Detect(Config{}); err == nil {
		t.Error("an empty config has no light")
	}
}

func steppedLight(t *testing.T, rawMax string, raw string) (*Sysfs, string) {
	t.Helper()
	glob := fakeBacklight(t, "bl", rawMax, raw)
	l, err := Detect(Config{Sysfs: glob, Steps: 24, Gamma: 2})
	if err != nil {
		t.Fatal(err)
	}
	s, ok := l.(*Sysfs)
	if !ok {
		t.Fatalf("got %T", l)
	}
	return s, filepath.Join(filepath.Dir(glob), "bl", "brightness")
}

func TestStepsHideTheRawRange(t *testing.T) {
	l, _ := steppedLight(t, "4095", "0")
	if l.Max() != 24 {
		t.Fatalf("max = %d, want 24 steps, not the raw 4095", l.Max())
	}
	// Off is off, full is full, and everything between rises steadily.
	if l.raw(0) != 0 || l.raw(24) != 4095 {
		t.Errorf("ends: raw(0)=%d raw(24)=%d", l.raw(0), l.raw(24))
	}
	prev := -1
	for n := 0; n <= 24; n++ {
		r := l.raw(n)
		if r <= prev {
			t.Errorf("step %d raw %d does not rise above step %d raw %d", n, r, n-1, prev)
		}
		prev = r
	}
	// The lowest step must be visibly lit: at least 1/256 of full.
	if l.raw(1) < 4095/256 {
		t.Errorf("step 1 is raw %d, too dim to count as on", l.raw(1))
	}
	// Squared: the middle step is about a quarter of the raw range.
	if r := l.raw(12); r < 900 || r > 1100 {
		t.Errorf("step 12 is raw %d, want about a quarter of 4095", r)
	}
}

func TestStepsRoundTripThroughTheFile(t *testing.T) {
	l, file := steppedLight(t, "4095", "0")
	for n := 0; n <= 24; n++ {
		if err := l.Set(n); err != nil {
			t.Fatal(err)
		}
		if got, err := l.Level(); err != nil || got != n {
			t.Errorf("Set(%d) then Level() = %d, %v", n, got, err)
		}
	}
	l.Set(24)
	if b, _ := os.ReadFile(file); string(b) != "4095" {
		t.Errorf("the file holds %q at the top step, want the raw maximum 4095", b)
	}
	l.Set(99) // clamped to the top step
	if n, _ := l.Level(); n != 24 {
		t.Errorf("Set(99) read back as %d", n)
	}
}

func TestStepsReadAValueTheStockSoftwareSet(t *testing.T) {
	// Raw values not on our grid read as the nearest step.
	l, file := steppedLight(t, "4095", "4095")
	os.WriteFile(file, []byte("1000\n"), 0o644)
	if n, _ := l.Level(); n != 12 {
		t.Errorf("raw 1000 reads as step %d, want 12 (raw 1024)", n)
	}
	os.WriteFile(file, []byte("0\n"), 0o644)
	if n, _ := l.Level(); n != 0 {
		t.Errorf("raw 0 reads as step %d", n)
	}
	os.WriteFile(file, []byte("5000\n"), 0o644) // beyond the maximum
	if n, _ := l.Level(); n != 24 {
		t.Errorf("raw 5000 reads as step %d", n)
	}
}

func TestStepsNotUsedWhenTheRawRangeIsAlreadySmall(t *testing.T) {
	// A light with only 10 levels asked for 24 steps keeps its own 10.
	glob := fakeBacklight(t, "bl", "10", "0")
	l, err := Detect(Config{Sysfs: glob, Steps: 24})
	if err != nil || l.Max() != 10 {
		t.Errorf("max = %d, %v; want the raw 10", l.Max(), err)
	}
}

func TestAFineRawRangeGetsTheDefaultStepsWithNoConfig(t *testing.T) {
	// The device's profile.json predates the steps setting and an app update
	// does not replace it: it names no steps, and the answer must still be 24.
	glob := fakeBacklight(t, "bl", "4095", "0")
	l, err := Detect(Config{Sysfs: glob})
	if err != nil || l.Max() != DefaultSteps {
		t.Fatalf("max = %d, %v; want the default %d steps", l.Max(), err, DefaultSteps)
	}
	if err := l.Set(DefaultSteps); err != nil {
		t.Fatal(err)
	}
	if n, _ := l.Level(); n != DefaultSteps {
		t.Errorf("top step reads back as %d", n)
	}
}

func TestNegativeStepsMeansTheRawRange(t *testing.T) {
	glob := fakeBacklight(t, "bl", "4095", "0")
	l, err := Detect(Config{Sysfs: glob, Steps: -1})
	if err != nil || l.Max() != 4095 {
		t.Errorf("max = %d, %v; want the raw 4095", l.Max(), err)
	}
}

func TestExplicitStepsOverrideTheDefault(t *testing.T) {
	glob := fakeBacklight(t, "bl", "4095", "0")
	l, _ := Detect(Config{Sysfs: glob, Steps: 10})
	if l.Max() != 10 {
		t.Errorf("max = %d, want the configured 10", l.Max())
	}
}
