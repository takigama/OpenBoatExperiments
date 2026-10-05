package web

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"signalkpaperdisplay/internal/app"
	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/frontlight"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// fake is a Controller that remembers what it was asked.
type fake struct {
	c     app.Control
	paths []signalk.PathInfo
	calls []string
	fail  map[string]error
}

func newFake() *fake {
	return &fake{c: app.Control{
		Page: "compass", Pages: []app.Choice{{ID: "compass", Name: "Compass"}, {ID: "nav", Name: "Numbers"}},
		Boxes: pages.DefaultBoxes(), LightMax: 24, Light: 6, Speed: "sog", Server: "10.0.0.76:3001", Connected: true,
	}}
}

func (f *fake) Control() app.Control      { return f.c }
func (f *fake) Paths() []signalk.PathInfo { return f.paths }
func (f *fake) rec(s string, err error) error {
	f.calls = append(f.calls, s)
	if e := f.fail[s]; e != nil {
		return e
	}
	return err
}
func (f *fake) ChoosePage(id string) error { return f.rec("page "+id, nil) }
func (f *fake) SetInvert(on bool)          { f.rec(fmt.Sprintf("invert %v", on), nil) }
func (f *fake) SetBox(i int, k string) error {
	return f.rec(fmt.Sprintf("box %d %s", i, k), nil)
}
func (f *fake) SetBrightness(l int) error     { return f.rec(fmt.Sprintf("brightness %d", l), nil) }
func (f *fake) SetWindTrue(on bool)           { f.rec(fmt.Sprintf("wind %v", on), nil) }
func (f *fake) SetSpeed(k string) error       { return f.rec("speed "+k, nil) }
func (f *fake) SetDepth(k string) error       { return f.rec("depth "+k, nil) }
func (f *fake) SetMapRange(nm int) error      { return f.rec(fmt.Sprintf("maprange %d", nm), nil) }
func (f *fake) SetMapNorthUp(on bool)         { f.rec(fmt.Sprintf("mapnorth %v", on), nil) }
func (f *fake) SetNoPowerMinutes(m int) error { return f.rec(fmt.Sprintf("nopower %d", m), nil) }
func (f *fake) Wake()                         { f.rec("wake", nil) }
func (f *fake) SetDemoMode(on bool)           { f.rec(fmt.Sprintf("demo %v", on), nil) }
func (f *fake) SetServer(h string) error      { return f.rec("server "+h, nil) }
func (f *fake) SetUnits(p string, o map[string]string) error {
	return f.rec(fmt.Sprintf("units %s %v", p, o), nil)
}

func serve(t *testing.T, f *fake, token string) *httptest.Server {
	t.Helper()
	s := httptest.NewServer((&Server{App: f, Token: token, Version: "43", Platform: "kindle-test", AllowConfig: true}).Handler())
	t.Cleanup(s.Close)
	return s
}

func post(t *testing.T, s *httptest.Server, body string, hdr map[string]string) (*http.Response, map[string]any) {
	t.Helper()
	req, _ := http.NewRequest("POST", s.URL+"/api/control", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var out map[string]any
	json.NewDecoder(resp.Body).Decode(&out)
	return resp, out
}

func TestStateDescribesEverythingTheScreenCanDo(t *testing.T) {
	f := newFake()
	f.c.Speed = pages.PathKindID("environment.outside.pressure")
	f.c.Boxes[3] = pages.PathKindID("propulsion.main.oilPressure")
	s := serve(t, f, "")
	resp, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st stateJSON
	if err := json.NewDecoder(resp.Body).Decode(&st); err != nil {
		t.Fatal(err)
	}
	if st.Version != "43" || st.Platform != "kindle-test" || st.Page != "compass" || len(st.Pages) != 2 || !st.Connected {
		t.Errorf("basics: %+v", st)
	}
	if len(st.Boxes) != pages.NavBoxes || st.Limits.Boxes != pages.NavBoxes {
		t.Errorf("boxes: %v", st.Boxes)
	}
	if st.Light == nil || st.Light.Max != 24 || st.Light.Level != 6 {
		t.Errorf("light: %+v", st.Light)
	}
	hasAIS := func(ks []kindJSON) bool {
		for _, k := range ks {
			if k.ID == pages.BoxAIS {
				return true
			}
		}
		return false
	}
	if !hasAIS(st.Kinds) || hasAIS(st.Speed.Options) {
		t.Error("a box can show the closest ship but the speed widget cannot")
	}
	if len(st.Speed.Options) != len(pages.BoxKinds)-1 {
		t.Errorf("speed options = %d, want every kind but one (%d)", len(st.Speed.Options), len(pages.BoxKinds)-1)
	}
	if len(st.PathKinds) != 2 || st.PathKinds[0].Label == "" {
		t.Errorf("the paths in use should be named: %+v", st.PathKinds)
	}
	// No front light: reported as null, so the page hides the slider.
	f.c.LightMax = 0
	resp2, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp2.Body.Close()
	var st2 stateJSON
	json.NewDecoder(resp2.Body).Decode(&st2)
	if st2.Light != nil {
		t.Errorf("a device with no light: %+v", st2.Light)
	}
}

func TestThePageIsServedSafely(t *testing.T) {
	s := serve(t, newFake(), "")
	resp, err := http.Get(s.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b := new(strings.Builder)
	buf := make([]byte, 4096)
	for {
		n, err := resp.Body.Read(buf)
		b.Write(buf[:n])
		if err != nil {
			break
		}
	}
	if resp.StatusCode != 200 || !strings.Contains(b.String(), "Dashboard control") || !strings.Contains(b.String(), "/api/control") {
		t.Errorf("status %d, body %d bytes", resp.StatusCode, b.Len())
	}
	if csp := resp.Header.Get("Content-Security-Policy"); !strings.Contains(csp, "default-src 'self'") || !strings.Contains(csp, "frame-ancestors 'none'") {
		t.Errorf("CSP = %q", csp)
	}
	if resp.Header.Get("Cache-Control") != "no-store" || resp.Header.Get("X-Content-Type-Options") != "nosniff" {
		t.Error("missing no-store / nosniff")
	}
	if r, _ := http.Get(s.URL + "/nope"); r.StatusCode != 404 {
		t.Errorf("an unknown path: %d", r.StatusCode)
	}
	// The page must not pull anything from elsewhere: it works on a boat with no internet.
	for _, bad := range []string{"http://", "https://", "//cdn"} {
		if strings.Contains(strings.ReplaceAll(b.String(), "http://www.w3.org", ""), bad) {
			t.Errorf("the page refers to %q: it must be self-contained", bad)
		}
	}
}

func TestControlAppliesEachChange(t *testing.T) {
	f := newFake()
	s := serve(t, f, "")
	resp, out := post(t, s, `{"page":"nav","invert":true,"brightness":12,"windTrue":true,"speed":"depth","boxes":{"0":"baro","7":"path:vendor.x"}}`, nil)
	if resp.StatusCode != 200 {
		t.Fatalf("status %d: %v", resp.StatusCode, out)
	}
	want := []string{"page nav", "invert true", "box 0 baro", "box 7 path:vendor.x", "brightness 12", "wind true", "speed depth"}
	if strings.Join(f.calls, "|") != strings.Join(want, "|") {
		t.Errorf("calls = %v\nwant    %v", f.calls, want)
	}
	if out["page"] == nil {
		t.Errorf("the reply should be the new state: %v", out)
	}

	// A full list of boxes.
	f.calls = nil
	list := `["sog","stw","cog","hdg","vmgw","vmgwp","depth","wtemp"]`
	if resp, out := post(t, s, `{"boxes":`+list+`}`, nil); resp.StatusCode != 200 {
		t.Fatalf("a full list: %d %v", resp.StatusCode, out)
	}
	if len(f.calls) != pages.NavBoxes || f.calls[1] != "box 1 stw" {
		t.Errorf("a list sets every box: %v", f.calls)
	}

	// Only what is sent is changed.
	f.calls = nil
	post(t, s, `{"invert":false}`, nil)
	if len(f.calls) != 1 || f.calls[0] != "invert false" {
		t.Errorf("one field sent, calls = %v", f.calls)
	}
	f.calls = nil
	post(t, s, `{}`, nil)
	if len(f.calls) != 0 {
		t.Errorf("an empty change did something: %v", f.calls)
	}
}

func TestControlChangesNothingUnlessEverythingIsGood(t *testing.T) {
	for name, body := range map[string]string{
		"bad page":          `{"invert":true,"page":"nope"}`,
		"bad kind":          `{"invert":true,"boxes":{"0":"nope"}}`,
		"bad path kind":     `{"invert":true,"boxes":{"0":"path:a b"}}`,
		"bad box number":    `{"invert":true,"boxes":{"8":"sog"}}`,
		"negative box":      `{"invert":true,"boxes":{"-1":"sog"}}`,
		"text box number":   `{"invert":true,"boxes":{"x":"sog"}}`,
		"short list":        `{"invert":true,"boxes":["sog"]}`,
		"boxes of nonsense": `{"invert":true,"boxes":7}`,
		"brightness high":   `{"invert":true,"brightness":25}`,
		"brightness low":    `{"invert":true,"brightness":-1}`,
		"speed AIS":         `{"invert":true,"speed":"ais1"}`,
		"speed unknown":     `{"invert":true,"speed":"nope"}`,
		"unknown field":     `{"invert":true,"power":"off"}`,
		"not json":          `invert`,
		"wrong type":        `{"invert":"yes"}`,
	} {
		f := newFake()
		s := serve(t, f, "")
		resp, out := post(t, s, body, nil)
		if resp.StatusCode != 400 {
			t.Errorf("%s: status %d, want 400", name, resp.StatusCode)
		}
		if out["errors"] == nil {
			t.Errorf("%s: no errors in the reply: %v", name, out)
		}
		if len(f.calls) != 0 {
			t.Errorf("%s: something was applied although part of it was bad: %v", name, f.calls)
		}
	}
}

func TestControlSaysWhichFieldWasWrong(t *testing.T) {
	s := serve(t, newFake(), "")
	_, out := post(t, s, `{"page":"nope","speed":"nope","brightness":99}`, nil)
	errs, _ := out["errors"].(map[string]any)
	for _, k := range []string{"page", "speed", "brightness"} {
		if errs[k] == nil {
			t.Errorf("no error named for %s: %v", k, errs)
		}
	}
	// A device with no light refuses a brightness.
	f := newFake()
	f.c.LightMax = 0
	_, out = post(t, serve(t, f, ""), `{"brightness":3}`, nil)
	if e, _ := out["errors"].(map[string]any); e["brightness"] == nil {
		t.Errorf("no light: %v", out)
	}
}

func TestAChangeThatFailsToApplyIsReported(t *testing.T) {
	f := newFake()
	f.fail = map[string]error{"brightness 5": errors.New("front light: no such device")}
	s := serve(t, f, "")
	resp, out := post(t, s, `{"brightness":5,"invert":true}`, nil)
	if resp.StatusCode != 500 {
		t.Errorf("status %d, want 500", resp.StatusCode)
	}
	if e, _ := out["errors"].(map[string]any); e["brightness"] == nil || out["state"] == nil {
		t.Errorf("reply: %v", out)
	}
}

func TestOnlyJSONFromThisSiteCanChangeAnything(t *testing.T) {
	f := newFake()
	s := serve(t, f, "")

	// A form on another web page would send this.
	req, _ := http.NewRequest("POST", s.URL+"/api/control", strings.NewReader("invert=true"))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	if resp, _ := http.DefaultClient.Do(req); resp.StatusCode != 415 {
		t.Errorf("a form post: %d, want 415", resp.StatusCode)
	}
	// Plain text pretending to be JSON in a "simple" cross-site request.
	req, _ = http.NewRequest("POST", s.URL+"/api/control", strings.NewReader(`{"invert":true}`))
	req.Header.Set("Content-Type", "text/plain")
	if resp, _ := http.DefaultClient.Do(req); resp.StatusCode != 415 {
		t.Errorf("text/plain: %d, want 415", resp.StatusCode)
	}
	// JSON, but from a page on another site.
	if resp, _ := post(t, s, `{"invert":true}`, map[string]string{"Origin": "http://evil.example"}); resp.StatusCode != 403 {
		t.Errorf("a cross-site request: %d, want 403", resp.StatusCode)
	}
	// From this site's own page.
	host := strings.TrimPrefix(s.URL, "http://")
	if resp, _ := post(t, s, `{"invert":true}`, map[string]string{"Origin": "http://" + host}); resp.StatusCode != 200 {
		t.Errorf("this site's own page: %d, want 200", resp.StatusCode)
	}
	if len(f.calls) != 1 {
		t.Errorf("only the one good request should have applied: %v", f.calls)
	}
	// Methods.
	for _, tc := range []struct{ method, path string }{{"GET", "/api/control"}, {"POST", "/api/state"}, {"POST", "/api/paths"}, {"POST", "/"}, {"PUT", "/api/control"}} {
		req, _ := http.NewRequest(tc.method, s.URL+tc.path, strings.NewReader("{}"))
		req.Header.Set("Content-Type", "application/json")
		if resp, _ := http.DefaultClient.Do(req); resp.StatusCode != 405 {
			t.Errorf("%s %s: %d, want 405", tc.method, tc.path, resp.StatusCode)
		}
	}
	// A body past the limit is refused, not read to the end.
	big := `{"speed":"` + strings.Repeat("a", maxBody+10) + `"}`
	if resp, _ := post(t, s, big, nil); resp.StatusCode != 400 {
		t.Errorf("an oversize body: %d, want 400", resp.StatusCode)
	}
}

func TestPathsListsWhatTheServerSends(t *testing.T) {
	f := newFake()
	now := time.Now()
	f.paths = []signalk.PathInfo{
		{Path: "environment.outside.pressure", Value: 101300, At: now.Add(-2 * time.Second), Units: "Pa"},
		{Path: "navigation.speedOverGround", Value: 3, At: now},
		{Path: "propulsion.main.oilPressure", Value: 302000, At: now},
	}
	s := serve(t, f, "")
	get := func(q string) []pathJSON {
		resp, err := http.Get(s.URL + "/api/paths" + q)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		var out struct{ Paths []pathJSON }
		json.NewDecoder(resp.Body).Decode(&out)
		return out.Paths
	}
	all := get("")
	if len(all) != 3 || all[0].Path != "environment.outside.pressure" || all[0].Units != "Pa" || all[0].Age < 1.5 || all[0].Age > 5 {
		t.Errorf("all: %+v", all)
	}
	if got := get("?q=PRESS"); len(got) != 2 {
		t.Errorf("a search ignores case and matches inside: %+v", got)
	}
	if got := get("?q=zzz"); len(got) != 0 {
		t.Errorf("nothing matches: %+v", got)
	}
	if body := get("?q=zzz"); body == nil {
		t.Error("an empty list should be [] not null")
	}
}

func TestTokenProtectsEverything(t *testing.T) {
	f := newFake()
	s := serve(t, f, "s3cret")
	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	do := func(method, path, body string, hdr map[string]string) *http.Response {
		req, _ := http.NewRequest(method, s.URL+path, strings.NewReader(body))
		if body != "" {
			req.Header.Set("Content-Type", "application/json")
		}
		for k, v := range hdr {
			req.Header.Set(k, v)
		}
		resp, err := client.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		return resp
	}
	for _, path := range []string{"/", "/api/state", "/api/paths"} {
		if r := do("GET", path, "", nil); r.StatusCode != 401 {
			t.Errorf("GET %s without a token: %d, want 401", path, r.StatusCode)
		}
	}
	if r := do("POST", "/api/control", `{"invert":true}`, nil); r.StatusCode != 401 {
		t.Errorf("POST without a token: %d", r.StatusCode)
	}
	if len(f.calls) != 0 {
		t.Fatalf("something was applied without a token: %v", f.calls)
	}
	for _, wrong := range []string{"s3cre", "s3crets", "S3CRET", "x"} {
		if r := do("GET", "/api/state", "", map[string]string{"Authorization": "Bearer " + wrong}); r.StatusCode != 401 {
			t.Errorf("wrong token %q accepted", wrong)
		}
	}
	if r := do("GET", "/api/state", "", map[string]string{"Authorization": "Bearer s3cret"}); r.StatusCode != 200 {
		t.Errorf("bearer: %d", r.StatusCode)
	}
	if r := do("POST", "/api/control", `{"invert":true}`, map[string]string{"Authorization": "Bearer s3cret"}); r.StatusCode != 200 {
		t.Errorf("bearer post: %d", r.StatusCode)
	}

	// Opening the page with the token once leaves a cookie and takes it out of the address.
	r := do("GET", "/?token=s3cret", "", nil)
	if r.StatusCode != http.StatusSeeOther || r.Header.Get("Location") != "/" {
		t.Fatalf("token in the address: %d -> %q", r.StatusCode, r.Header.Get("Location"))
	}
	var cookie *http.Cookie
	for _, c := range r.Cookies() {
		if c.Name == cookieName {
			cookie = c
		}
	}
	if cookie == nil || !cookie.HttpOnly || cookie.SameSite != http.SameSiteStrictMode {
		t.Fatalf("cookie: %+v", cookie)
	}
	if r := do("GET", "/api/state", "", map[string]string{"Cookie": cookie.Name + "=" + cookie.Value}); r.StatusCode != 200 {
		t.Errorf("with the cookie: %d", r.StatusCode)
	}
	if r := do("GET", "/?token=wrong", "", nil); r.StatusCode != 401 {
		t.Errorf("a wrong token in the address: %d", r.StatusCode)
	}
}

// The real thing: HTTP in, the real App changed, nothing faked.
func TestEndToEndAgainstTheRealApp(t *testing.T) {
	dir := t.TempDir()
	a := &app.App{
		State:   signalk.NewState(),
		Display: &display.PNG{W: 1072, H: 1448, Path: filepath.Join(dir, "f.png")},
		Units:   units.Settings{Preset: units.PresetMetric}, SettingsPath: filepath.Join(dir, "settings.json"),
		Light: frontlight.NewFake(24, 3),
	}
	a.InitLight()
	a.SetPage("compass")
	srv := httptest.NewServer((&Server{App: a, Version: "43"}).Handler())
	defer srv.Close()

	resp, out := post(t, srv, `{"page":"nav","invert":true,"brightness":10,"windTrue":true,"speed":"path:environment.outside.pressure","boxes":{"0":"baro","1":"path:vendor.x"}}`, nil)
	if resp.StatusCode != 200 {
		t.Fatalf("%d %v", resp.StatusCode, out)
	}
	c := a.Control()
	if c.Page != "nav" || !c.Invert || c.Light != 10 || !c.WindTrue || c.Speed != "path:environment.outside.pressure" || c.Boxes[0] != "baro" || c.Boxes[1] != "path:vendor.x" {
		t.Errorf("the app was not changed as asked: %+v", c)
	}
	r2, err := http.Get(srv.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer r2.Body.Close()
	var st stateJSON
	json.NewDecoder(r2.Body).Decode(&st)
	if st.Page != "nav" || !st.Invert || st.Light.Level != 10 || !st.Wind.True || len(st.PathKinds) != 2 {
		t.Errorf("the state does not say so: %+v", st)
	}
}

func TestDepthWidgetThroughTheAPI(t *testing.T) {
	f := newFake()
	f.c.Depth = "depth"
	s := serve(t, f, "")

	resp, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st stateJSON
	json.NewDecoder(resp.Body).Decode(&st)
	if st.Depth.Kind != "depth" || len(st.Depth.Options) != len(pages.BoxKinds)-1 {
		t.Errorf("depth widget in the state: %+v", st.Depth)
	}
	for _, k := range st.Depth.Options {
		if k.ID == pages.BoxAIS {
			t.Error("the depth widget cannot show the closest-ship box")
		}
	}

	if resp, out := post(t, s, `{"depth":"path:environment.outside.pressure"}`, nil); resp.StatusCode != 200 || out["depth"] == nil {
		t.Fatalf("%d %v", resp.StatusCode, out)
	}
	if len(f.calls) != 1 || f.calls[0] != "depth path:environment.outside.pressure" {
		t.Errorf("calls = %v", f.calls)
	}
	// Bad, with something good alongside: nothing is applied.
	f.calls = nil
	for _, bad := range []string{"nope", "ais1", "path:a b"} {
		resp, out := post(t, s, `{"invert":true,"depth":"`+bad+`"}`, nil)
		if resp.StatusCode != 400 {
			t.Errorf("%s: status %d", bad, resp.StatusCode)
		}
		if e, _ := out["errors"].(map[string]any); e["depth"] == nil {
			t.Errorf("%s: no error named for depth: %v", bad, out)
		}
	}
	if len(f.calls) != 0 {
		t.Errorf("something was applied despite the bad request: %v", f.calls)
	}
}

func TestConfigThroughTheAPI(t *testing.T) {
	f := newFake()
	f.c.DefaultServer = "10.0.0.76:3001"
	s := serve(t, f, "")

	resp, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st stateJSON
	json.NewDecoder(resp.Body).Decode(&st)
	if !st.Config || st.DefaultServer != "10.0.0.76:3001" || st.Server != "10.0.0.76:3001" {
		t.Errorf("config state: %+v", st)
	}
	if st.Units.Preset != "metric" || len(st.Units.Presets) != 3 || len(st.Units.Metrics) == 0 {
		t.Fatalf("units: %+v", st.Units)
	}
	var sog metricJSON
	for _, m := range st.Units.Metrics {
		if m.ID == "sog" {
			sog = m
		}
	}
	if sog.Label == "" || sog.Unit != "km/h" || sog.Overridden || len(sog.Options) < 3 {
		t.Errorf("a metric: %+v", sog)
	}

	body := `{"demo":true,"server":"10.0.0.5","units":{"preset":"nautical","overrides":{"depth":"ft"}}}`
	if resp, out := post(t, s, body, nil); resp.StatusCode != 200 {
		t.Fatalf("%d %v", resp.StatusCode, out)
	}
	want := []string{"units nautical map[depth:ft]", "server 10.0.0.5", "demo true"}
	if strings.Join(f.calls, "|") != strings.Join(want, "|") {
		t.Errorf("calls = %v, want %v", f.calls, want)
	}
	// An empty server means the default; units can be only a preset.
	f.calls = nil
	post(t, s, `{"server":"","units":{"preset":"metric"},"demo":false}`, nil)
	if len(f.calls) != 3 || f.calls[1] != "server " {
		t.Errorf("an empty server should be passed on as empty: %v", f.calls)
	}
}

func TestConfigIsCheckedAsAWholeToo(t *testing.T) {
	for name, body := range map[string]string{
		"bad server":       `{"invert":true,"server":"not a host"}`,
		"bad port":         `{"invert":true,"server":"10.0.0.5:99999"}`,
		"bad preset":       `{"invert":true,"units":{"preset":"nope"}}`,
		"bad metric":       `{"invert":true,"units":{"overrides":{"nope":"kn"}}}`,
		"bad unit":         `{"invert":true,"units":{"overrides":{"sog":"furlongs"}}}`,
		"unit wrong kind":  `{"invert":true,"units":{"overrides":{"depth":"kn"}}}`,
		"unknown in units": `{"invert":true,"units":{"colour":"red"}}`,
		"demo not bool":    `{"demo":"yes"}`,
	} {
		f := newFake()
		s := serve(t, f, "")
		resp, out := post(t, s, body, nil)
		if resp.StatusCode != 400 || out["errors"] == nil {
			t.Errorf("%s: %d %v", name, resp.StatusCode, out)
		}
		if len(f.calls) != 0 {
			t.Errorf("%s: something was applied: %v", name, f.calls)
		}
	}
	_, out := post(t, serve(t, newFake(), ""), `{"server":"x y","units":{"preset":"nope"}}`, nil)
	e, _ := out["errors"].(map[string]any)
	if e["server"] == nil || e["units"] == nil {
		t.Errorf("each wrong field should be named: %v", e)
	}
}

func TestConfigCanBeTurnedOff(t *testing.T) {
	f := newFake()
	srv := httptest.NewServer((&Server{App: f, Version: "43", AllowConfig: false}).Handler())
	defer srv.Close()
	for _, body := range []string{`{"demo":true}`, `{"server":"10.0.0.5"}`, `{"units":{"preset":"imperial"}}`, `{"invert":true,"demo":true}`} {
		resp, out := post(t, srv, body, nil)
		if resp.StatusCode != 403 {
			t.Errorf("%s: status %d, want 403", body, resp.StatusCode)
		}
		if e, _ := out["errors"].(map[string]any); e["config"] == nil {
			t.Errorf("%s: no config error: %v", body, out)
		}
	}
	if len(f.calls) != 0 {
		t.Errorf("a refused request applied something: %v", f.calls)
	}
	// What is on screen can still be changed.
	if resp, _ := post(t, srv, `{"invert":true}`, nil); resp.StatusCode != 200 || len(f.calls) != 1 {
		t.Errorf("display controls should still work: %d %v", resp.StatusCode, f.calls)
	}
	r, err := http.Get(srv.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer r.Body.Close()
	var st stateJSON
	json.NewDecoder(r.Body).Decode(&st)
	if st.Config {
		t.Error("the state should say config is off, so the page leaves the controls out")
	}
}

func TestMapThroughTheAPI(t *testing.T) {
	f := newFake()
	f.c.MapRange = 5
	f.c.MapRanges = []int{1, 2, 5, 10}
	f.c.Pages = append(f.c.Pages, app.Choice{ID: "map", Name: "Map"})
	s := serve(t, f, "")

	resp, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st stateJSON
	json.NewDecoder(resp.Body).Decode(&st)
	if st.Map.Range != 5 || st.Map.NorthUp || len(st.Map.Ranges) != 4 {
		t.Errorf("map in the state: %+v", st.Map)
	}

	if resp, out := post(t, s, `{"page":"map","mapRange":10,"mapNorthUp":true}`, nil); resp.StatusCode != 200 || out["map"] == nil {
		t.Fatalf("%d %v", resp.StatusCode, out)
	}
	want := []string{"page map", "maprange 10", "mapnorth true"}
	if strings.Join(f.calls, "|") != strings.Join(want, "|") {
		t.Errorf("calls = %v, want %v", f.calls, want)
	}
	// A range the map does not have is refused, with the rest of the request.
	f.calls = nil
	for _, bad := range []string{"3", "0", "-1", "50"} {
		resp, out := post(t, s, `{"invert":true,"mapRange":`+bad+`}`, nil)
		if resp.StatusCode != 400 {
			t.Errorf("range %s: status %d", bad, resp.StatusCode)
		}
		if e, _ := out["errors"].(map[string]any); e["mapRange"] == nil {
			t.Errorf("range %s: no error named: %v", bad, out)
		}
	}
	if resp, _ := post(t, s, `{"mapRange":"ten"}`, nil); resp.StatusCode != 400 {
		t.Errorf("a text range: %d", resp.StatusCode)
	}
	if len(f.calls) != 0 {
		t.Errorf("something was applied despite the bad request: %v", f.calls)
	}
}

func TestNoPowerThroughTheAPI(t *testing.T) {
	f := newFake()
	f.c.NoPowerMin = 60
	s := serve(t, f, "")

	resp, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st stateJSON
	json.NewDecoder(resp.Body).Decode(&st)
	if st.NoPower.Minutes != 60 || st.NoPower.Active || len(st.NoPower.Options) != len(pages.NoPowerChoices) {
		t.Errorf("no-power in the state: %+v", st.NoPower)
	}
	if o := st.NoPower.Options; o[0].Minutes != 1 || o[len(o)-1].Minutes != 0 || o[len(o)-1].Label != "Never" {
		t.Errorf("options run from 1 minute to never: %+v", o)
	}

	for _, ok := range []string{"0", "1", "90", "10080"} {
		f.calls = nil
		if resp, out := post(t, s, `{"noPowerMinutes":`+ok+`}`, nil); resp.StatusCode != 200 {
			t.Errorf("%s: %d %v", ok, resp.StatusCode, out)
		}
		if len(f.calls) != 1 || f.calls[0] != "nopower "+ok {
			t.Errorf("%s: calls = %v", ok, f.calls)
		}
	}
	// Bad ones are refused with the rest of the request.
	f.calls = nil
	for _, bad := range []string{"-1", "10081", "2147483648", "1.5", "\"60\""} {
		resp, out := post(t, s, `{"invert":true,"noPowerMinutes":`+bad+`}`, nil)
		if resp.StatusCode != 400 {
			t.Errorf("%s: status %d, want 400", bad, resp.StatusCode)
		}
		_ = out
	}
	if _, out := post(t, s, `{"noPowerMinutes":-5}`, nil); out["errors"] == nil {
		t.Errorf("no error named: %v", out)
	} else if e, _ := out["errors"].(map[string]any); e["noPowerMinutes"] == nil {
		t.Errorf("the field is not named: %v", e)
	}
	if len(f.calls) != 0 {
		t.Errorf("something was applied despite the bad request: %v", f.calls)
	}

	// Waking it.
	f.calls = nil
	post(t, s, `{"wake":true}`, nil)
	if len(f.calls) != 1 || f.calls[0] != "wake" {
		t.Errorf("wake: %v", f.calls)
	}
	f.calls = nil
	post(t, s, `{"wake":false}`, nil)
	if len(f.calls) != 0 {
		t.Errorf("wake false should do nothing: %v", f.calls)
	}
	// While it is showing, the state says so (the page shows a banner).
	f.c.NoPower = true
	r2, err := http.Get(s.URL + "/api/state")
	if err != nil {
		t.Fatal(err)
	}
	defer r2.Body.Close()
	var st2 stateJSON
	json.NewDecoder(r2.Body).Decode(&st2)
	if !st2.NoPower.Active {
		t.Error("the state should say NO POWER is showing")
	}

	// A display setting, like invert: it works with config changes switched off.
	g := newFake()
	srv := httptest.NewServer((&Server{App: g, Version: "43", AllowConfig: false}).Handler())
	defer srv.Close()
	if resp, _ := post(t, srv, `{"noPowerMinutes":15,"wake":true}`, nil); resp.StatusCode != 200 || len(g.calls) != 2 {
		t.Errorf("with config off: %d %v", resp.StatusCode, g.calls)
	}
}
