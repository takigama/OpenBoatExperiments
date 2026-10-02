package battery

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// supply makes a power_supply directory with the given files.
func supply(t *testing.T, root, name string, files map[string]string) {
	t.Helper()
	d := filepath.Join(root, name)
	if err := os.MkdirAll(d, 0o755); err != nil {
		t.Fatal(err)
	}
	for f, v := range files {
		if err := os.WriteFile(filepath.Join(d, f), []byte(v+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func TestSysfsOnBattery(t *testing.T) {
	root := t.TempDir()
	supply(t, root, "max77696-battery", map[string]string{"type": "Battery", "capacity": "73", "status": "Discharging"})
	supply(t, root, "max77696-charger", map[string]string{"type": "Mains", "online": "0"})
	r, err := NewSysfs(root)
	if err != nil {
		t.Fatal(err)
	}
	st, err := r.Read()
	if err != nil || st.Percent != 73 || st.Plugged || st.Charging {
		t.Errorf("got %+v, %v; want 73%% on battery", st, err)
	}
}

func TestSysfsCharging(t *testing.T) {
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "41", "status": "Charging"})
	supply(t, root, "usb", map[string]string{"type": "USB", "online": "1"})
	r, _ := NewSysfs(root)
	st, _ := r.Read()
	if st.Percent != 41 || !st.Plugged || !st.Charging {
		t.Errorf("got %+v, want 41%% charging and plugged", st)
	}
}

func TestSysfsFullAndPlugged(t *testing.T) {
	// Full on a charger: plugged in, but no longer charging.
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "100", "status": "Full"})
	r, _ := NewSysfs(root)
	if st, _ := r.Read(); st.Percent != 100 || !st.Plugged || st.Charging {
		t.Errorf("got %+v, want 100%% plugged, not charging", st)
	}
}

func TestSysfsChargerThatOnlyReportsOnline(t *testing.T) {
	// The battery says nothing useful about charging, but the charger is online.
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "60", "status": "Unknown"})
	supply(t, root, "chg", map[string]string{"type": "Mains", "online": "1"})
	r, _ := NewSysfs(root)
	if st, _ := r.Read(); !st.Plugged || st.Percent != 60 {
		t.Errorf("got %+v, want plugged at 60%%", st)
	}
	// A charger with its own "charging" flag.
	root2 := t.TempDir()
	supply(t, root2, "bat", map[string]string{"type": "Battery", "capacity": "30"})
	supply(t, root2, "chg", map[string]string{"type": "Mains", "online": "0", "charging": "1"})
	r2, _ := NewSysfs(root2)
	if st, _ := r2.Read(); !st.Plugged || !st.Charging {
		t.Errorf("got %+v, want charging via the charger's flag", st)
	}
}

func TestSysfsClampsAndReportsErrors(t *testing.T) {
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "140"})
	r, _ := NewSysfs(root)
	if st, _ := r.Read(); st.Percent != 100 {
		t.Errorf("140 should clamp to 100, got %d", st.Percent)
	}
	os.Remove(filepath.Join(root, "bat", "capacity"))
	if _, err := r.Read(); err == nil {
		t.Error("a vanished capacity file is an error")
	}
	if _, err := NewSysfs(t.TempDir()); err == nil {
		t.Error("no power supplies at all should be an error")
	}
	// Only non-batteries: no battery to read.
	only := t.TempDir()
	supply(t, only, "usb", map[string]string{"type": "USB", "online": "1"})
	if _, err := NewSysfs(only); err == nil {
		t.Error("a USB supply alone is not a battery")
	}
}

func TestLipc(t *testing.T) {
	answers := map[string]string{"battLevel": "88", "isCharging": "1"}
	l := &Lipc{run: func(_ context.Context, name string, args ...string) (string, error) {
		return answers[args[len(args)-1]] + "\n", nil
	}}
	if st, err := l.Read(); err != nil || st.Percent != 88 || !st.Charging || !st.Plugged {
		t.Errorf("got %+v, %v", st, err)
	}
	answers["isCharging"] = "0"
	if st, _ := l.Read(); st.Charging || st.Plugged {
		t.Errorf("not charging: %+v", st)
	}
	l.run = func(context.Context, string, ...string) (string, error) { return "", errors.New("exit status 1") }
	if _, err := l.Read(); err == nil {
		t.Error("a failing lipc is an error")
	}
	l.run = func(context.Context, string, ...string) (string, error) { return "banana", nil }
	if _, err := l.Read(); err == nil {
		t.Error("a non-numeric level is an error")
	}
}

func TestDischargingBatteryOverridesASupplyThatStaysOnline(t *testing.T) {
	// The cable is out and the battery says Discharging, but a USB supply still
	// reports online: the battery is right.
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "88", "status": "Discharging"})
	supply(t, root, "usb", map[string]string{"type": "USB", "online": "1"})
	supply(t, root, "chg", map[string]string{"type": "Mains", "online": "1", "charging": "1"})
	r, _ := NewSysfs(root)
	if st, _ := r.Read(); st.Plugged || st.Charging || st.Percent != 88 {
		t.Errorf("got %+v, want 88%% on battery, not plugged", st)
	}
}

func TestNotChargingMeansPluggedInButIdle(t *testing.T) {
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "80", "status": "Not charging"})
	r, _ := NewSysfs(root)
	if st, _ := r.Read(); !st.Plugged || st.Charging {
		t.Errorf("got %+v, want plugged in but not charging", st)
	}
}

func TestUnknownStatusFallsBackToTheOtherSupplies(t *testing.T) {
	// No decisive word from the battery: the other supplies decide, both ways.
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "50", "status": "Unknown"})
	supply(t, root, "usb", map[string]string{"type": "USB", "online": "1"})
	r, _ := NewSysfs(root)
	if st, _ := r.Read(); !st.Plugged {
		t.Errorf("an online USB supply with an unknown battery status means plugged in: %+v", st)
	}
	os.WriteFile(filepath.Join(root, "usb", "online"), []byte("0\n"), 0o644)
	if st, _ := r.Read(); st.Plugged {
		t.Errorf("with the supply offline too it is not plugged in: %+v", st)
	}
}

func TestUnplugging(t *testing.T) {
	// The whole sequence the Kindle goes through, read each step.
	root := t.TempDir()
	supply(t, root, "bat", map[string]string{"type": "Battery", "capacity": "99", "status": "Charging"})
	supply(t, root, "usb", map[string]string{"type": "USB", "online": "1"})
	r, _ := NewSysfs(root)
	steps := []struct {
		status            string
		online            string
		plugged, charging bool
	}{
		{"Charging", "1", true, true},
		{"Full", "1", true, false},
		{"Discharging", "1", false, false}, // cable out, supply slow to report it
		{"Discharging", "0", false, false},
	}
	for i, s := range steps {
		os.WriteFile(filepath.Join(root, "bat", "status"), []byte(s.status+"\n"), 0o644)
		os.WriteFile(filepath.Join(root, "usb", "online"), []byte(s.online+"\n"), 0o644)
		if st, _ := r.Read(); st.Plugged != s.plugged || st.Charging != s.charging {
			t.Errorf("step %d (%s, online %s): %+v, want plugged=%v charging=%v", i, s.status, s.online, st, s.plugged, s.charging)
		}
	}
}
