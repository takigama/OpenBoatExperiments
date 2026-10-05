// Package web is a small web page and JSON API for controlling the dashboard
// from a phone or a laptop: which page shows, what is in the Nav boxes, invert,
// the backlight, and the compass widgets. It does what the touch screen does,
// through the same App methods, and nothing the touch screen cannot (no server
// address, no power).
//
// It speaks plain HTTP on a boat's local network, like the SignalK server it
// reads from. Without a token anyone on that network can use it; with one
// (-web-token) every request needs it, as an Authorization: Bearer header, or
// once as ?token= in the address, which sets a cookie.
package web

import (
	"context"
	"crypto/subtle"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"signalkpaperdisplay/internal/app"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

//go:embed index.html
var indexHTML []byte

// Controller is the dashboard, as the web page sees it. *app.App is one.
type Controller interface {
	Control() app.Control
	Paths() []signalk.PathInfo
	ChoosePage(id string) error
	SetInvert(on bool)
	SetBox(i int, kind string) error
	SetBrightness(level int) error
	SetWindTrue(on bool)
	SetSpeed(kind string) error
	SetDepth(kind string) error
	SetMapRange(nm int) error
	SetMapNorthUp(on bool)
	SetNoPowerMinutes(minutes int) error
	Wake()
	SetDemoMode(on bool)
	SetServer(hostPort string) error
	SetUnits(preset string, overrides map[string]string) error
	SetTimezone(name string) error
	SetIdleSwitch(enabled bool, path string) error
	UpdateNow() error
}

// Server serves the control page and API.
type Server struct {
	App      Controller
	Token    string // if set, required on every request
	Version  string // the app's build number, shown on the page
	Platform string // the device profile's name, shown on the page
	// AllowConfig lets the page and API change the configuration: demo mode, the
	// SignalK server and the units, beyond what is on screen. Off, a request that
	// tries is refused and the page leaves those controls out.
	AllowConfig bool
}

const (
	maxBody    = 64 << 10
	cookieName = "pdtoken"
)

// Handler is the whole site.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/state", s.state)
	mux.HandleFunc("/api/paths", s.paths)
	mux.HandleFunc("/api/control", s.control)
	mux.HandleFunc("/", s.index)
	return s.guard(mux)
}

// ListenAndServe serves on addr until ctx ends.
func (s *Server) ListenAndServe(ctx context.Context, addr string) error {
	srv := &http.Server{
		Addr:              addr,
		Handler:           s.Handler(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       30 * time.Second,
		MaxHeaderBytes:    16 << 10,
	}
	go func() {
		<-ctx.Done()
		sh, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		srv.Shutdown(sh)
	}()
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	log.Printf("web: control page on %s", addr)
	if err := srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	return nil
}

// guard checks the token, and that a change is not being made by another web
// page the user happens to have open (a cross-site request): the browser says
// where such a request came from, and it must be this very site.
func (s *Server) guard(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		if r.Method == http.MethodPost {
			if o := r.Header.Get("Origin"); o != "" {
				if u, err := url.Parse(o); err != nil || u.Host != r.Host {
					http.Error(w, "cross-site request refused", http.StatusForbidden)
					return
				}
			}
		}
		if s.Token != "" {
			given := bearer(r)
			if q := r.URL.Query().Get("token"); q != "" {
				given = q
			}
			if given == "" {
				if c, err := r.Cookie(cookieName); err == nil {
					given = c.Value
				}
			}
			if subtle.ConstantTimeCompare([]byte(given), []byte(s.Token)) != 1 {
				w.Header().Set("WWW-Authenticate", `Bearer realm="paperdisplay"`)
				http.Error(w, "a token is needed: open this page as /?token=...", http.StatusUnauthorized)
				return
			}
			if q := r.URL.Query().Get("token"); q != "" && r.Method == http.MethodGet && r.URL.Path == "/" {
				// Remember it, and take it out of the address (and so out of the
				// history and anything that shares the link).
				http.SetCookie(w, &http.Cookie{Name: cookieName, Value: s.Token, Path: "/", HttpOnly: true, SameSite: http.SameSiteStrictMode, MaxAge: 365 * 24 * 3600})
				http.Redirect(w, r, "/", http.StatusSeeOther)
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

func bearer(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if v, ok := strings.CutPrefix(h, "Bearer "); ok {
		return strings.TrimSpace(v)
	}
	return ""
}

func (s *Server) index(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'")
	w.Write(indexHTML)
}

// --- the state ---------------------------------------------------------------

type kindJSON struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Label string `json:"label"`
}

type stateJSON struct {
	Version  string      `json:"version"`
	Platform string      `json:"platform"`
	Page     string      `json:"page"`
	Pages    []kindJSON  `json:"pages"`
	Invert   bool        `json:"invert"`
	Boxes    []string    `json:"boxes"`
	Kinds    []kindJSON  `json:"kinds"` // what a Nav box can show
	Light    *lightJSON  `json:"light"` // nil: no front light
	Wind     windJSON    `json:"wind"`
	Speed    speedJSON   `json:"speed"`
	Depth    speedJSON   `json:"depth"`
	Map      mapJSON     `json:"map"`
	NoPower  noPowerJSON `json:"noPower"`
	Demo     bool        `json:"demo"`
	Server   string      `json:"server"`
	// DefaultServer is what the server goes back to when it is cleared.
	DefaultServer string     `json:"defaultServer"`
	Units         unitsJSON  `json:"units"`
	Config        bool       `json:"config"` // the page may change demo mode, the server and the units
	Connected     bool       `json:"connected"`
	PathKinds     []kindJSON `json:"pathKinds,omitempty"` // the paths in use, named
	Limits        limitsJSON `json:"limits"`
	Time          timeJSON   `json:"time"`
	Idle          idleJSON   `json:"idle"`
	Update        updateJSON `json:"update"`
}

// timeJSON is the clock's time zone ("" is the device's own), the time of day it
// gives, and the zones the screen offers (the page accepts any name the zone
// database knows).
type timeJSON struct {
	Zone  string   `json:"zone"`
	Clock string   `json:"clock"`
	Zones []string `json:"zones"`
}

// idleJSON is idle mode: whether it is on, the SignalK switch it watches and the
// default for that, what the switch last said ("on", "off", "unknown") and whether
// the screen is idle now.
type idleJSON struct {
	Enabled     bool   `json:"enabled"`
	Path        string `json:"path"`
	DefaultPath string `json:"defaultPath"`
	Switch      string `json:"switch"`
	Active      bool   `json:"active"`
}

// updateJSON is the software update: whether this display can do one, whether a
// check is running, and what the last one found.
type updateJSON struct {
	Can       bool   `json:"can"`
	Busy      bool   `json:"busy"`
	Message   string `json:"message"`
	Available bool   `json:"available"`
}

type lightJSON struct {
	Level int `json:"level"`
	Max   int `json:"max"`
}

type windJSON struct {
	True bool `json:"true"`
}

type speedJSON struct {
	Kind    string     `json:"kind"`
	Options []kindJSON `json:"options"` // what the speed widget can show
}

// mapJSON is the map page's range (nautical miles), the ranges there are, and
// whether north is up.
// noPowerJSON is the no-power mode: after how many minutes off external power the
// screen goes to NO POWER (0 is never), the timeouts to offer, and whether it is
// showing now.
type noPowerJSON struct {
	Minutes int             `json:"minutes"`
	Active  bool            `json:"active"`
	Options []noPowerOption `json:"options"`
}

type noPowerOption struct {
	Minutes int    `json:"minutes"`
	Label   string `json:"label"`
}

type mapJSON struct {
	Range   int   `json:"range"`
	Ranges  []int `json:"ranges"`
	NorthUp bool  `json:"northUp"`
}

type unitsJSON struct {
	Preset  string       `json:"preset"`
	Presets []string     `json:"presets"`
	Metrics []metricJSON `json:"metrics"`
}

// metricJSON is one value with its own unit setting.
type metricJSON struct {
	ID         string   `json:"id"`
	Label      string   `json:"label"`
	Unit       string   `json:"unit"` // the one in use
	Overridden bool     `json:"overridden"`
	Options    []string `json:"options"`
}

type limitsJSON struct {
	Boxes int `json:"boxes"`
}

func (s *Server) stateNow() stateJSON {
	c := s.App.Control()
	st := stateJSON{
		Version: s.Version, Platform: s.Platform, Page: c.Page, Invert: c.Invert, Boxes: c.Boxes,
		Wind: windJSON{True: c.WindTrue}, Speed: speedJSON{Kind: c.Speed}, Depth: speedJSON{Kind: c.Depth}, Map: mapJSON{Range: c.MapRange, Ranges: c.MapRanges, NorthUp: c.MapNorthUp},
		NoPower: noPowerJSON{Minutes: c.NoPowerMin, Active: c.NoPower},
		Demo:    c.Demo, Server: c.Server, DefaultServer: c.DefaultServer, Connected: c.Connected, Config: s.AllowConfig,
		Units:  unitsState(c.Units),
		Limits: limitsJSON{Boxes: pages.NavBoxes},
		Time:   timeJSON{Zone: c.Timezone, Clock: c.Clock},
		Idle:   idleJSON{Enabled: c.IdleEnabled, Path: c.IdlePath, DefaultPath: settings.DefaultIdlePath, Switch: c.IdleSwitch, Active: c.Idle},
		Update: updateJSON{Can: c.CanUpdate, Busy: c.UpdateBusy, Message: c.UpdateMsg, Available: c.UpdateAvailable},
	}
	for _, z := range pages.ZoneChoices {
		if z.Name != "" {
			st.Time.Zones = append(st.Time.Zones, z.Name)
		}
	}
	for _, p := range c.Pages {
		st.Pages = append(st.Pages, kindJSON{ID: p.ID, Name: p.Name})
	}
	for _, o := range pages.NoPowerChoices {
		st.NoPower.Options = append(st.NoPower.Options, noPowerOption{Minutes: o.Minutes, Label: o.Label})
	}
	for _, k := range pages.BoxKinds {
		kj := kindJSON{ID: k.ID, Name: k.Name, Label: k.Label}
		st.Kinds = append(st.Kinds, kj)
		if k.ID != pages.BoxAIS {
			st.Speed.Options = append(st.Speed.Options, kj)
			st.Depth.Options = append(st.Depth.Options, kj)
		}
	}
	if c.LightMax > 0 {
		st.Light = &lightJSON{Level: c.Light, Max: c.LightMax}
	}
	// Paths in use that are not in the built-in list, so the page can name them.
	seen := map[string]bool{}
	for _, id := range append(append([]string(nil), c.Boxes...), c.Speed, c.Depth) {
		if pages.IsPathKind(id) && !seen[id] {
			seen[id] = true
			k, _ := pages.BoxKindByID(id)
			st.PathKinds = append(st.PathKinds, kindJSON{ID: id, Name: k.Name, Label: k.Label})
		}
	}
	return st
}

func unitsState(u units.Settings) unitsJSON {
	out := unitsJSON{Preset: u.Preset, Presets: units.Presets()}
	if out.Preset == "" {
		out.Preset = units.PresetMetric
	}
	for _, m := range units.Metrics {
		mj := metricJSON{ID: m.ID, Label: m.Label, Unit: u.UnitFor(m.ID).Symbol, Overridden: u.IsOverridden(m.ID)}
		for _, o := range units.Units(m.Qty) {
			mj.Options = append(mj.Options, o.Symbol)
		}
		out.Metrics = append(out.Metrics, mj)
	}
	return out
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func (s *Server) state(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	writeJSON(w, http.StatusOK, s.stateNow())
}

type pathJSON struct {
	Path  string  `json:"path"`
	Value float64 `json:"value"`
	Age   float64 `json:"age"` // seconds since it last arrived
	Units string  `json:"units,omitempty"`
}

// paths lists the numeric SignalK paths the server has sent. ?q= keeps those
// whose path contains the text, ignoring case.
func (s *Server) paths(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	q := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("q")))
	now := time.Now()
	out := []pathJSON{}
	for _, p := range s.App.Paths() {
		if q != "" && !strings.Contains(strings.ToLower(p.Path), q) {
			continue
		}
		if _, ok := pages.BoxKindByID(pages.PathKindID(p.Path)); !ok {
			continue // a name the dashboard could not use anyway
		}
		out = append(out, pathJSON{Path: p.Path, Value: p.Value, Age: now.Sub(p.At).Seconds(), Units: p.Units})
	}
	writeJSON(w, http.StatusOK, map[string]any{"paths": out})
}

// --- changing it -------------------------------------------------------------

// controlReq is a change: every field is optional, and only the ones present are
// applied. boxes is either a full list of NavBoxes kind IDs, or an object from box
// number (counting from 0, left to right then top to bottom) to kind ID.
type controlReq struct {
	Page       *string         `json:"page"`
	Invert     *bool           `json:"invert"`
	Boxes      json.RawMessage `json:"boxes"`
	Brightness *int            `json:"brightness"`
	WindTrue   *bool           `json:"windTrue"`
	Speed      *string         `json:"speed"`
	Depth      *string         `json:"depth"`
	// The map page: its range in nautical miles (one of the ranges in /api/state),
	// and whether north is up instead of our heading.
	MapRange   *int  `json:"mapRange"`
	MapNorthUp *bool `json:"mapNorthUp"`
	// NoPowerMinutes is how long off external power before the screen goes to NO
	// POWER: 0 for never, or 1 to a week's worth of minutes. wake brings it back
	// from NO POWER for another timeout.
	NoPowerMinutes *int  `json:"noPowerMinutes"`
	Wake           *bool `json:"wake"`
	// The configuration. server is "host" or "host:port" (port 3000 if left out),
	// or "" for the default; units is a preset (which resets every unit) and/or
	// overrides, from a metric ID (see /api/state) to a unit symbol.
	Demo   *bool     `json:"demo"`
	Server *string   `json:"server"`
	Units  *unitsReq `json:"units"`
	// timezone is an IANA name such as "Australia/Sydney", or "" for the device's
	// own; idle turns idle mode on or off and/or chooses the switch it watches (""
	// for the default); update true looks for a newer release and installs it.
	Timezone *string  `json:"timezone"`
	Idle     *idleReq `json:"idle"`
	Update   *bool    `json:"update"`
}

type idleReq struct {
	Enabled *bool   `json:"enabled"`
	Path    *string `json:"path"`
}

type unitsReq struct {
	Preset    string            `json:"preset"`
	Overrides map[string]string `json:"overrides"`
}

func (s *Server) control(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	// Only a script or this very page can send JSON like this: a form on some
	// other site cannot, and a fetch from one would need permission it won't get.
	if ct := r.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		http.Error(w, "send application/json", http.StatusUnsupportedMediaType)
		return
	}
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxBody))
	dec.DisallowUnknownFields()
	var req controlReq
	if err := dec.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]any{"errors": map[string]string{"body": err.Error()}})
		return
	}

	errs := map[string]string{}
	c := s.App.Control()

	if req.Page != nil {
		ok := false
		for _, p := range c.Pages {
			ok = ok || p.ID == *req.Page
		}
		if !ok {
			errs["page"] = fmt.Sprintf("no page %q", *req.Page)
		}
	}

	boxes := map[int]string{} // box index -> kind
	if len(req.Boxes) > 0 && string(req.Boxes) != "null" {
		var list []string
		var byIndex map[string]string
		switch {
		case json.Unmarshal(req.Boxes, &list) == nil:
			if len(list) != pages.NavBoxes {
				errs["boxes"] = fmt.Sprintf("a list needs %d kinds, got %d", pages.NavBoxes, len(list))
			}
			for i, k := range list {
				boxes[i] = k
			}
		case json.Unmarshal(req.Boxes, &byIndex) == nil:
			for is, k := range byIndex {
				i, err := strconv.Atoi(is)
				if err != nil || i < 0 || i >= pages.NavBoxes {
					errs["boxes"] = fmt.Sprintf("there is no box %q (0 to %d)", is, pages.NavBoxes-1)
					continue
				}
				boxes[i] = k
			}
		default:
			errs["boxes"] = "boxes is a list of kinds, or an object from box number to kind"
		}
		for i, k := range boxes {
			if !app.ValidBox(k) {
				errs["boxes"] = fmt.Sprintf("box %d cannot show %q", i, k)
			}
		}
	}

	if req.Brightness != nil {
		switch {
		case c.LightMax == 0:
			errs["brightness"] = "this device has no front light"
		case *req.Brightness < 0 || *req.Brightness > c.LightMax:
			errs["brightness"] = fmt.Sprintf("brightness is 0 to %d", c.LightMax)
		}
	}
	if req.Speed != nil {
		if _, ok := pages.SpeedFromID(*req.Speed); !ok {
			errs["speed"] = fmt.Sprintf("the speed widget cannot show %q", *req.Speed)
		}
	}

	if req.NoPowerMinutes != nil && !settings.ValidNoPower(*req.NoPowerMinutes) {
		errs["noPowerMinutes"] = fmt.Sprintf("no-power mode is 0 (never) or 1 to %d minutes", settings.MaxNoPowerMinutes)
	}
	if req.MapRange != nil && !pages.MapRangeOK(*req.MapRange) {
		errs["mapRange"] = fmt.Sprintf("the map has no %d nm range (%v)", *req.MapRange, pages.MapRanges)
	}
	if req.Depth != nil {
		if _, ok := pages.DepthFromID(*req.Depth); !ok {
			errs["depth"] = fmt.Sprintf("the depth widget cannot show %q", *req.Depth)
		}
	}

	if req.Demo != nil || req.Server != nil || req.Units != nil || req.Timezone != nil || req.Idle != nil || req.Update != nil {
		if !s.AllowConfig {
			// Refused whole: not "demo mode but not the server".
			writeJSON(w, http.StatusForbidden, map[string]any{"errors": map[string]string{"config": "changing demo mode, the server, the units, the time zone, idle mode or updating from here is turned off (-web-config=false)"}})
			return
		}
	}
	if req.Timezone != nil {
		if _, err := settings.ParseTimezone(*req.Timezone); err != nil {
			errs["timezone"] = err.Error()
		}
	}
	if req.Idle != nil && req.Idle.Path != nil && *req.Idle.Path != "" {
		if _, ok := signalk.MetaPath(*req.Idle.Path); !ok {
			errs["idle"] = fmt.Sprintf("%q is not a SignalK path like %s", *req.Idle.Path, settings.DefaultIdlePath)
		}
	}
	if req.Update != nil && *req.Update && !c.CanUpdate {
		errs["update"] = "this display cannot update itself"
	}
	if req.Server != nil && strings.TrimSpace(*req.Server) != "" {
		if _, err := settings.NormalizeServer(*req.Server); err != nil {
			errs["server"] = fmt.Sprintf("%q: %v", *req.Server, err)
		}
	}
	if req.Units != nil {
		// Tried on a copy of the current settings: the same checks the app makes.
		u := c.Units.Clone()
		if req.Units.Preset != "" {
			if err := u.SetPreset(req.Units.Preset); err != nil {
				errs["units"] = err.Error()
			}
		}
		for id, sym := range req.Units.Overrides {
			if err := u.SetUnit(id, sym); err != nil {
				errs["units"] = err.Error()
			}
		}
	}

	if len(errs) > 0 { // nothing is applied unless all of it is good
		writeJSON(w, http.StatusBadRequest, map[string]any{"errors": errs})
		return
	}

	if req.Page != nil {
		if err := s.App.ChoosePage(*req.Page); err != nil {
			errs["page"] = err.Error()
		}
	}
	if req.Invert != nil {
		s.App.SetInvert(*req.Invert)
	}
	for i := 0; i < pages.NavBoxes; i++ {
		if k, ok := boxes[i]; ok {
			if err := s.App.SetBox(i, k); err != nil {
				errs["boxes"] = err.Error()
			}
		}
	}
	if req.Brightness != nil {
		if err := s.App.SetBrightness(*req.Brightness); err != nil {
			errs["brightness"] = err.Error()
		}
	}
	if req.WindTrue != nil {
		s.App.SetWindTrue(*req.WindTrue)
	}
	if req.Speed != nil {
		if err := s.App.SetSpeed(*req.Speed); err != nil {
			errs["speed"] = err.Error()
		}
	}
	if req.Depth != nil {
		if err := s.App.SetDepth(*req.Depth); err != nil {
			errs["depth"] = err.Error()
		}
	}
	if req.Wake != nil && *req.Wake {
		s.App.Wake()
	}
	if req.NoPowerMinutes != nil {
		if err := s.App.SetNoPowerMinutes(*req.NoPowerMinutes); err != nil {
			errs["noPowerMinutes"] = err.Error()
		}
	}
	if req.MapRange != nil {
		if err := s.App.SetMapRange(*req.MapRange); err != nil {
			errs["mapRange"] = err.Error()
		}
	}
	if req.MapNorthUp != nil {
		s.App.SetMapNorthUp(*req.MapNorthUp)
	}
	if req.Units != nil {
		if err := s.App.SetUnits(req.Units.Preset, req.Units.Overrides); err != nil {
			errs["units"] = err.Error()
		}
	}
	if req.Server != nil {
		if err := s.App.SetServer(*req.Server); err != nil {
			errs["server"] = err.Error()
		}
	}
	if req.Timezone != nil {
		if err := s.App.SetTimezone(*req.Timezone); err != nil {
			errs["timezone"] = err.Error()
		}
	}
	if req.Idle != nil {
		enabled, path := c.IdleEnabled, c.IdleChosen // what is not mentioned stays
		if req.Idle.Enabled != nil {
			enabled = *req.Idle.Enabled
		}
		if req.Idle.Path != nil {
			path = *req.Idle.Path
		}
		if err := s.App.SetIdleSwitch(enabled, path); err != nil {
			errs["idle"] = err.Error()
		}
	}
	if req.Demo != nil {
		s.App.SetDemoMode(*req.Demo)
	}
	if req.Update != nil && *req.Update {
		if err := s.App.UpdateNow(); err != nil {
			errs["update"] = err.Error()
		}
	}
	if len(errs) > 0 { // something that passed the checks failed to apply, e.g. the light
		out := s.stateNow()
		writeJSON(w, http.StatusInternalServerError, map[string]any{"errors": errs, "state": out})
		return
	}
	writeJSON(w, http.StatusOK, s.stateNow())
}
