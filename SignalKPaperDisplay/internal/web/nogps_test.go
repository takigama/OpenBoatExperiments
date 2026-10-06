package web

import (
	"net/http"
	"net/http/httptest"
	"reflect"
	"testing"

	"signalkpaperdisplay/internal/settings"
)

func TestStateHasTheGPSTimeout(t *testing.T) {
	f := newFake()
	f.c.NoGPSSec = 30
	st := stateOf(t, serve(t, f, ""))
	if st.NoGPS.Seconds != 30 || st.NoGPS.Default != settings.DefaultNoGPSSeconds || len(st.NoGPS.Options) < 4 {
		t.Errorf("noGps: %+v", st.NoGPS)
	}
	if last := st.NoGPS.Options[len(st.NoGPS.Options)-1]; last.Seconds != 0 || last.Label != "Off" {
		t.Errorf("Off should be the last option: %+v", last)
	}
}

func TestGPSTimeoutChange(t *testing.T) {
	f := newFake()
	s := serve(t, f, "")
	for _, n := range []string{"10", "0", "2", "3600"} {
		if resp, _ := post(t, s, `{"noGpsSeconds":`+n+`}`, nil); resp.StatusCode != 200 {
			t.Errorf("%s: status %d", n, resp.StatusCode)
		}
	}
	want := []string{"nogps 10", "nogps 0", "nogps 2", "nogps 3600"}
	if !reflect.DeepEqual(f.calls, want) {
		t.Errorf("calls = %v, want %v", f.calls, want)
	}

	// Out of range: refused, and nothing else in the request is applied.
	f.calls = nil
	for _, bad := range []string{"1", "-5", "3601", "86400"} {
		resp, out := post(t, s, `{"noGpsSeconds":`+bad+`,"invert":true}`, nil)
		if resp.StatusCode != 400 {
			t.Errorf("%s: status %d, want 400", bad, resp.StatusCode)
		}
		if errs, _ := out["errors"].(map[string]any); errs["noGpsSeconds"] == nil {
			t.Errorf("%s: the error should name the field: %v", bad, out)
		}
	}
	if len(f.calls) != 0 {
		t.Errorf("a refused request did something: %v", f.calls)
	}
}

// Like no-power mode, this is a display setting, not configuration: it works with the
// configuration switch off.
func TestGPSTimeoutNeedsNoConfigPermission(t *testing.T) {
	f := newFake()
	srv := httptest.NewServer((&Server{App: f, Version: "52", AllowConfig: false}).Handler())
	defer srv.Close()
	resp, _ := post(t, srv, `{"noGpsSeconds":20}`, nil)
	if resp.StatusCode != http.StatusOK || !reflect.DeepEqual(f.calls, []string{"nogps 20"}) {
		t.Errorf("status %d, calls %v", resp.StatusCode, f.calls)
	}
}
