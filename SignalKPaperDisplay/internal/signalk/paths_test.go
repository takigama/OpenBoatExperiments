package signalk

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func delta(path, value string) []byte {
	return []byte(fmt.Sprintf(`{"context":"vessels.self","updates":[{"values":[{"path":%q,"value":%s}]}]}`, path, value))
}

func sendPath(t *testing.T, st *State, at time.Time, path, value string) {
	t.Helper()
	c := &Client{State: st}
	c.handle(delta(path, value), at)
}

func TestCatalogKeepsEveryNumericPathOfOurOwnVessel(t *testing.T) {
	st := NewState()
	now := time.Now()
	sendPath(t, st, now, "navigation.speedOverGround", "3.2") // one the dashboard knows
	sendPath(t, st, now, "vendor.custom.thing", "7.5")        // one it does not
	sendPath(t, st, now, "navigation.attitude", `{"roll":0.1,"pitch":0.2,"yaw":3.0}`)
	sendPath(t, st, now, "steering.autopilot.state", `"auto"`) // not a number
	sendPath(t, st, now, "navigation.position", `{"latitude":-33.8,"longitude":151.2}`)
	sendPath(t, st, now, "environment.nothing", `null`)

	got := map[string]float64{}
	for _, p := range st.Catalog() {
		got[p.Path] = p.Value
	}
	for path, want := range map[string]float64{
		"navigation.speedOverGround": 3.2, "vendor.custom.thing": 7.5,
		"navigation.attitude.roll": 0.1, "navigation.attitude.pitch": 0.2, "navigation.attitude.yaw": 3.0,
		"navigation.position.latitude": -33.8,
	} {
		if v, ok := got[path]; !ok || v != want {
			t.Errorf("%s = %v (present %v), want %v", path, v, ok, want)
		}
	}
	for _, not := range []string{"steering.autopilot.state", "environment.nothing"} {
		if _, ok := got[not]; ok {
			t.Errorf("%s is not a number and should not be listed", not)
		}
	}
	// Sorted by name, so the list is stable.
	cat := st.Catalog()
	for i := 1; i < len(cat); i++ {
		if cat[i-1].Path > cat[i].Path {
			t.Fatalf("not sorted: %s before %s", cat[i-1].Path, cat[i].Path)
		}
	}
}

func TestOtherVesselsAreNotInTheCatalog(t *testing.T) {
	st := NewState()
	c := &Client{State: st}
	c.handle([]byte(`{"context":"vessels.urn:mrn:imo:mmsi:235000001","updates":[{"values":[{"path":"navigation.speedOverGround","value":4}]}]}`), time.Now())
	if len(st.Catalog()) != 0 {
		t.Errorf("a ship's data is in our own catalog: %v", st.Catalog())
	}
}

func TestCatalogIsBounded(t *testing.T) {
	st := NewState()
	now := time.Now()
	for i := 0; i < maxCatalog+500; i++ {
		sendPath(t, st, now, fmt.Sprintf("vendor.flood.n%d", i), "1")
	}
	if n := len(st.Catalog()); n != maxCatalog {
		t.Errorf("catalog holds %d paths, want it capped at %d", n, maxCatalog)
	}
	// A path already known still updates when the catalog is full.
	sendPath(t, st, now, "vendor.flood.n0", "42")
	for _, p := range st.Catalog() {
		if p.Path == "vendor.flood.n0" && p.Value != 42 {
			t.Errorf("a known path stopped updating: %v", p.Value)
		}
	}
}

func TestWatchedPathsAreCopiedIntoSnapshots(t *testing.T) {
	st := NewState()
	now := time.Now()
	sendPath(t, st, now.Add(-time.Hour), "vendor.slow.thing", "5") // arrived long ago, before it was wanted
	sendPath(t, st, now, "vendor.other", "6")

	if snap := st.Snapshot(); len(snap.Own.Watched) != 0 {
		t.Errorf("nothing is watched yet, but snapshot holds %v", snap.Own.Watched)
	}
	added := st.Watch([]string{"vendor.slow.thing", "vendor.never.sent"})
	if len(added) != 2 {
		t.Errorf("both paths are new to the watch list, got %v", added)
	}
	if again := st.Watch([]string{"vendor.slow.thing", "vendor.never.sent"}); len(again) != 0 {
		t.Errorf("watching the same again adds nothing, got %v", again)
	}
	snap := st.Snapshot()
	r, ok := snap.Own.Path("vendor.slow.thing")
	if !ok || r.V != 5 || !r.Valid() {
		t.Errorf("a path wanted after it was sent should show at once: %+v %v", r, ok)
	}
	if r, ok := snap.Own.Path("vendor.never.sent"); !ok || r.Valid() {
		t.Errorf("a path never sent is watched but has no value: %+v %v", r, ok)
	}
	if _, ok := snap.Own.Path("vendor.other"); ok {
		t.Error("a path nobody asked for should not be in the snapshot")
	}

	st.Watch([]string{"vendor.other"})
	if _, ok := st.Snapshot().Own.Path("vendor.slow.thing"); ok {
		t.Error("a path no longer wanted should leave the snapshot")
	}
}

func TestStalenessFollowsThePathsOwnRhythm(t *testing.T) {
	st := NewState()
	st.Watch([]string{"vendor.fast", "vendor.slow", "vendor.once"})
	t0 := time.Now().Add(-time.Hour)
	for i := 0; i < 10; i++ { // once a second
		sendPath(t, st, t0.Add(time.Duration(i)*time.Second), "vendor.fast", "1")
	}
	for i := 0; i < 5; i++ { // every minute
		sendPath(t, st, t0.Add(time.Duration(i)*time.Minute), "vendor.slow", "1")
	}
	sendPath(t, st, t0, "vendor.once", "1")

	snap := st.Snapshot()
	fast, _ := snap.Own.Path("vendor.fast")
	slow, _ := snap.Own.Path("vendor.slow")
	once, _ := snap.Own.Path("vendor.once")
	if fast.MaxAge != 5*time.Second {
		t.Errorf("a path arriving every second goes stale after the floor of 5s, got %v", fast.MaxAge)
	}
	if slow.MaxAge != 2*time.Minute {
		t.Errorf("a path arriving every minute is capped at 2 minutes, got %v", slow.MaxAge)
	}
	if once.MaxAge != 30*time.Second {
		t.Errorf("a path seen once has no rhythm yet and gets 30s, got %v", once.MaxAge)
	}
	now := t0.Add(9 * time.Second).Add(4 * time.Second)
	if !fast.Fresh(now) {
		t.Error("4 seconds after the last of a fast path it is still fresh")
	}
	if fast.Fresh(now.Add(10 * time.Second)) {
		t.Error("a fast path silent for 14 seconds must be stale: a frozen number looks live")
	}
}

func TestResetForgetsTheCatalogButKeepsWhatIsWatched(t *testing.T) {
	st := NewState()
	sendPath(t, st, time.Now(), "vendor.thing", "1")
	st.Watch([]string{"vendor.thing"})
	st.SetMeta("vendor.thing", "m/s")
	st.Reset()
	if len(st.Catalog()) != 0 || st.HasMeta("vendor.thing") {
		t.Error("a new server's paths and units are not the old one's")
	}
	if _, ok := st.Snapshot().Own.Path("vendor.thing"); !ok {
		t.Error("what to watch is the user's choice and should survive")
	}
}

func TestMetaPath(t *testing.T) {
	if p, ok := MetaPath("environment.wind.speedApparent"); !ok || p != "environment/wind/speedApparent/meta" {
		t.Errorf("MetaPath = %q %v", p, ok)
	}
	for _, bad := range []string{"", ".a", "a.", "a..b", "a/b", "a b", "a?b=c", "../x", strings.Repeat("a", 200), "a;b"} {
		if _, ok := MetaPath(bad); ok {
			t.Errorf("%q should be refused", bad)
		}
	}
}

func TestFetchMetaAsksTheServerForUnits(t *testing.T) {
	var asked string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		asked = r.URL.Path
		switch {
		case strings.Contains(r.URL.Path, "speedApparent"):
			w.Write([]byte(`{"units":"m/s","description":"x"}`))
		case strings.Contains(r.URL.Path, "boom"):
			http.Error(w, "no", http.StatusInternalServerError)
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()
	host := strings.TrimPrefix(srv.URL, "http://")
	st := NewState()
	c := &Client{URL: StreamURL(host), State: st}

	units, err := c.FetchMeta(context.Background(), "environment.wind.speedApparent")
	if err != nil || units != "m/s" {
		t.Fatalf("units = %q, err = %v", units, err)
	}
	if asked != "/signalk/v1/api/vessels/self/environment/wind/speedApparent/meta" {
		t.Errorf("asked for %q", asked)
	}
	if !st.HasMeta("environment.wind.speedApparent") {
		t.Error("the answer should be remembered")
	}

	// A path the server has no meta for is an answer too: no units, not asked again.
	if units, err := c.FetchMeta(context.Background(), "vendor.nothing"); err != nil || units != "" {
		t.Errorf("a 404 should mean no units, got %q, %v", units, err)
	}
	if !st.HasMeta("vendor.nothing") {
		t.Error("an empty answer should be remembered, or it is asked for again and again")
	}
	// A server error is not: try again later.
	if _, err := c.FetchMeta(context.Background(), "vendor.boom"); err == nil {
		t.Error("a server error should be an error")
	}
	if st.HasMeta("vendor.boom") {
		t.Error("a failure must not be remembered as an answer")
	}
	if _, err := c.FetchMeta(context.Background(), "bad path"); err == nil {
		t.Error("a path with odd characters must not be sent to the server")
	}
	if _, err := (&Client{State: st}).FetchMeta(context.Background(), "a.b"); err == nil {
		t.Error("with no server address there is nothing to ask")
	}
}
