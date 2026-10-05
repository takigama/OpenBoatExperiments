package web

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"

	"signalkpaperdisplay/internal/settings"
)

func stateOf(t *testing.T, s *httptest.Server) stateJSON {
	t.Helper()
	resp, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st stateJSON
	if err := json.NewDecoder(resp.Body).Decode(&st); err != nil {
		t.Fatal(err)
	}
	return st
}

func TestStateHasTheTimeIdleAndUpdateSettings(t *testing.T) {
	f := newFake()
	f.c.Timezone, f.c.Clock = "Australia/Sydney", "14:32"
	f.c.IdleEnabled, f.c.IdlePath, f.c.IdleSwitch, f.c.Idle = true, settings.DefaultIdlePath, "off", true
	f.c.CanUpdate, f.c.UpdateMsg = true, "Up to date: v50 is the newest"
	st := stateOf(t, serve(t, f, ""))

	if st.Time.Zone != "Australia/Sydney" || st.Time.Clock != "14:32" || len(st.Time.Zones) < 10 {
		t.Errorf("time: %+v", st.Time)
	}
	for _, z := range st.Time.Zones {
		if z == "" {
			t.Error("the empty zone is the device's own, not a name to offer")
		}
	}
	want := idleJSON{Enabled: true, Path: settings.DefaultIdlePath, DefaultPath: settings.DefaultIdlePath, Switch: "off", Active: true}
	if st.Idle != want {
		t.Errorf("idle = %+v, want %+v", st.Idle, want)
	}
	if !st.Update.Can || st.Update.Message == "" || st.Update.Busy {
		t.Errorf("update: %+v", st.Update)
	}
}

func TestTimezoneChange(t *testing.T) {
	f := newFake()
	s := serve(t, f, "")
	if resp, _ := post(t, s, `{"timezone":"Australia/Sydney"}`, nil); resp.StatusCode != 200 {
		t.Fatalf("status %d", resp.StatusCode)
	}
	if resp, _ := post(t, s, `{"timezone":""}`, nil); resp.StatusCode != 200 {
		t.Fatalf("clearing the zone: status %d", resp.StatusCode)
	}
	if !reflect.DeepEqual(f.calls, []string{"timezone Australia/Sydney", "timezone "}) {
		t.Errorf("calls = %v", f.calls)
	}

	// Not a zone: refused, and nothing else in the request is applied either.
	f.calls = nil
	resp, out := post(t, s, `{"timezone":"Mars/Olympus","invert":true}`, nil)
	if resp.StatusCode != 400 || len(f.calls) != 0 {
		t.Errorf("a bad zone: status %d, calls %v", resp.StatusCode, f.calls)
	}
	if errs, _ := out["errors"].(map[string]any); errs["timezone"] == nil {
		t.Errorf("the error should name the field: %v", out)
	}
	for _, bad := range []string{`../../etc/passwd`, `Australia/Sydney; rm`, strings.Repeat("a", 100)} {
		if resp, _ := post(t, s, `{"timezone":"`+bad+`"}`, nil); resp.StatusCode != 400 {
			t.Errorf("%q was accepted (status %d)", bad, resp.StatusCode)
		}
	}
}

func TestIdleChange(t *testing.T) {
	f := newFake()
	f.c.IdleEnabled, f.c.IdleChosen = false, "electrical.switches.nav.state"
	s := serve(t, f, "")

	// Turning it on keeps the switch that was chosen...
	post(t, s, `{"idle":{"enabled":true}}`, nil)
	// ...choosing a switch keeps whether it is on...
	f.c.IdleEnabled = true
	post(t, s, `{"idle":{"path":"electrical.switches.kindle.state"}}`, nil)
	// ...and "" goes back to the default.
	post(t, s, `{"idle":{"path":""}}`, nil)
	want := []string{
		"idle true electrical.switches.nav.state",
		"idle true electrical.switches.kindle.state",
		"idle true ",
	}
	if !reflect.DeepEqual(f.calls, want) {
		t.Errorf("calls = %v, want %v", f.calls, want)
	}

	f.calls = nil
	for _, bad := range []string{`has space`, `a..b`, `.lead`, `semi;colon`} {
		resp, _ := post(t, s, `{"idle":{"path":"`+bad+`"}}`, nil)
		if resp.StatusCode != 400 {
			t.Errorf("path %q was accepted (status %d)", bad, resp.StatusCode)
		}
	}
	if len(f.calls) != 0 {
		t.Errorf("a bad path changed something: %v", f.calls)
	}
}

func TestUpdateNow(t *testing.T) {
	f := newFake()
	s := serve(t, f, "")
	// A display that cannot update itself (a PC preview) says so.
	resp, out := post(t, s, `{"update":true}`, nil)
	if resp.StatusCode != 400 || len(f.calls) != 0 {
		t.Errorf("no updater: status %d, calls %v (%v)", resp.StatusCode, f.calls, out)
	}
	f.c.CanUpdate = true
	if resp, _ := post(t, s, `{"update":true}`, nil); resp.StatusCode != 200 {
		t.Fatalf("status %d", resp.StatusCode)
	}
	if !reflect.DeepEqual(f.calls, []string{"update"}) {
		t.Errorf("calls = %v", f.calls)
	}
	// "update": false is not a request to update.
	f.calls = nil
	post(t, s, `{"update":false}`, nil)
	if len(f.calls) != 0 {
		t.Errorf("update:false updated: %v", f.calls)
	}
}

func TestConfigSwitchRefusesTheNewSettingsToo(t *testing.T) {
	f := newFake()
	f.c.CanUpdate = true
	srv := httptest.NewServer((&Server{App: f, Version: "50", AllowConfig: false}).Handler())
	defer srv.Close()
	for _, body := range []string{`{"timezone":"UTC"}`, `{"idle":{"enabled":true}}`, `{"update":true}`} {
		resp, _ := post(t, srv, body, nil)
		if resp.StatusCode != http.StatusForbidden {
			t.Errorf("%s: status %d, want 403", body, resp.StatusCode)
		}
	}
	if len(f.calls) != 0 {
		t.Errorf("a refused request did something: %v", f.calls)
	}
}
