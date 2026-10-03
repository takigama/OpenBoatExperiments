package app

import (
	"os"
	"testing"
	"time"
)

func TestSetDemoMode(t *testing.T) {
	a, path := controlApp(t)
	var calls []bool
	a.OnDemoChange = func(on bool) { calls = append(calls, on) }

	a.SetDemoMode(true)
	if !a.Control().Demo || len(calls) != 1 || !calls[0] {
		t.Errorf("after on: Demo=%v calls=%v", a.Control().Demo, calls)
	}
	if !a.takePageChanged() {
		t.Error("the DEMO tag appears: a full refresh")
	}
	a.SetDemoMode(true) // already on
	if len(calls) != 1 {
		t.Errorf("switching on again told the feed again: %v", calls)
	}
	a.SetDemoMode(false)
	if a.Control().Demo || len(calls) != 2 || calls[1] {
		t.Errorf("after off: Demo=%v calls=%v", a.Control().Demo, calls)
	}
	a.SetDemoMode(false)
	if len(calls) != 2 {
		t.Errorf("switching off again told the feed again: %v", calls)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Error("demo mode is never saved: the app must start on the real server")
	}
	// No callback is fine.
	b, _ := controlApp(t)
	b.SetDemoMode(true)
	if !b.Control().Demo {
		t.Error("the switch should still flip")
	}
}

func TestSetServer(t *testing.T) {
	a, path := controlApp(t)
	a.DefaultServer = "10.0.0.76:3001"
	var moved []string
	a.OnServerChange = func(h string) { moved = append(moved, h) }

	if got := a.Control(); got.Server != "10.0.0.76:3001" || got.DefaultServer != "10.0.0.76:3001" {
		t.Fatalf("before: %+v", got)
	}

	// The port defaults to 3000, and it is saved.
	if err := a.SetServer("10.0.0.5"); err != nil {
		t.Fatal(err)
	}
	if a.Control().Server != "10.0.0.5:3000" || len(moved) != 1 || moved[0] != "10.0.0.5:3000" {
		t.Errorf("server = %q, moved = %v", a.Control().Server, moved)
	}
	if f := saved(t, path); f.Server != "10.0.0.5:3000" {
		t.Errorf("saved server = %q", f.Server)
	}
	if !a.takePageChanged() {
		t.Error("a new server is a full refresh (the header may change)")
	}

	// Asking for what it already is does nothing.
	if err := a.SetServer("10.0.0.5:3000"); err != nil || len(moved) != 1 {
		t.Errorf("same server again: %v, moved %v", err, moved)
	}
	if err := a.SetServer("  10.0.0.5:3000 "); err != nil || len(moved) != 1 {
		t.Errorf("spaces around it: %v, moved %v", err, moved)
	}

	// Bad addresses are refused and change nothing.
	for _, bad := range []string{"not a host", "10.0.0.5:99999", "10.0.0.5:abc", "300.1.1.1", "a/b", "host:", ":3000", "http://x"} {
		if err := a.SetServer(bad); err == nil {
			t.Errorf("%q should be refused", bad)
		}
	}
	if a.Control().Server != "10.0.0.5:3000" || len(moved) != 1 {
		t.Errorf("a refused address changed something: %q %v", a.Control().Server, moved)
	}

	// Empty goes back to the default - and the file says no choice was made, so a
	// later change to the launcher's setting still applies.
	if err := a.SetServer(""); err != nil {
		t.Fatal(err)
	}
	if a.Control().Server != "10.0.0.76:3001" || len(moved) != 2 || moved[1] != "10.0.0.76:3001" {
		t.Errorf("after clearing: %q, moved %v", a.Control().Server, moved)
	}
	if f := saved(t, path); f.Server != "" {
		t.Errorf("the default is not a choice, but the file holds %q", f.Server)
	}
	// Choosing the default by typing it is the same as having none.
	a.SetServer("10.0.0.5")
	a.SetServer("10.0.0.76:3001")
	if f := saved(t, path); f.Server != "" {
		t.Errorf("typing the default should store no choice, got %q", f.Server)
	}
	// A path-less hostname works too.
	if err := a.SetServer("signalk.local"); err != nil || a.Control().Server != "signalk.local:3000" {
		t.Errorf("hostname: %v %q", err, a.Control().Server)
	}
}

func TestSetServerForgetsWhatTheOldOneSaid(t *testing.T) {
	a, _ := controlApp(t)
	a.DefaultServer = "10.0.0.76:3001"
	asked := make(chan string, 4)
	a.FetchMeta = func(p string) { asked <- p }
	a.SetBox(0, "path:vendor.x")
	<-asked
	a.State.SetMeta("vendor.x", "m/s")
	a.SetServer("10.0.0.9")
	// The new server's units for the path are a new question.
	a.State.Reset() // what OnServerChange does in the real app
	a.syncWatch()
	select {
	case p := <-asked:
		if p != "vendor.x" {
			t.Errorf("asked about %q", p)
		}
	case <-time.After(time.Second):
		t.Error("after a server change the watched paths' units should be asked for again")
	}
}

func TestSetUnits(t *testing.T) {
	a, path := controlApp(t)
	if err := a.SetUnits("nautical", nil); err != nil {
		t.Fatal(err)
	}
	if a.Control().Units.Preset != "nautical" || a.Control().Units.UnitFor("sog").Symbol != "kn" {
		t.Errorf("preset: %+v", a.Control().Units)
	}
	if !a.takePageChanged() {
		t.Error("every number changes: a full refresh")
	}
	if f := saved(t, path); f.Preset != "nautical" {
		t.Errorf("saved preset = %q", f.Preset)
	}

	// Overrides on top of the preset.
	if err := a.SetUnits("", map[string]string{"depth": "m", "sog": "km/h"}); err != nil {
		t.Fatal(err)
	}
	u := a.Control().Units
	if u.UnitFor("depth").Symbol != "m" || u.UnitFor("sog").Symbol != "km/h" || u.UnitFor("stw").Symbol != "kn" {
		t.Errorf("overrides: %+v", u)
	}
	if f := saved(t, path); f.Overrides["depth"] != "m" || f.Overrides["sog"] != "km/h" {
		t.Errorf("saved overrides = %v", f.Overrides)
	}
	// Setting one back to what the preset gives drops the override.
	a.SetUnits("", map[string]string{"sog": "kn"})
	if a.Control().Units.IsOverridden("sog") {
		t.Error("an override equal to the preset should not count")
	}
	// A preset resets every override; with overrides alongside, they apply after it.
	a.SetUnits("imperial", map[string]string{"depth": "fm"})
	u = a.Control().Units
	if u.Preset != "imperial" || u.UnitFor("sog").Symbol != "mph" || u.UnitFor("depth").Symbol != "fm" {
		t.Errorf("preset then overrides: %+v", u)
	}

	// All or nothing.
	before := a.Control().Units
	for name, tc := range map[string]struct {
		preset string
		o      map[string]string
	}{
		"bad preset":    {"nope", nil},
		"bad metric":    {"", map[string]string{"nope": "kn"}},
		"bad unit":      {"", map[string]string{"sog": "furlongs"}},
		"wrong class":   {"", map[string]string{"depth": "kn"}},
		"good then bad": {"metric", map[string]string{"sog": "kn", "depth": "parsecs"}},
	} {
		if err := a.SetUnits(tc.preset, tc.o); err == nil {
			t.Errorf("%s should be refused", name)
		}
		after := a.Control().Units
		if after.Preset != before.Preset || len(after.Overrides) != len(before.Overrides) {
			t.Errorf("%s changed something: %+v -> %+v", name, before, after)
		}
	}
}
