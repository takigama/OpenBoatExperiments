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
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"signalkpaperdisplay/internal/app"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/signalk"
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
}

// Server serves the control page and API.
type Server struct {
	App      Controller
	Token    string // if set, required on every request
	Version  string // the app's build number, shown on the page
	Platform string // the device profile's name, shown on the page
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
	log.Printf("web: control page on %s", addr)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
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
	Version   string     `json:"version"`
	Platform  string     `json:"platform"`
	Page      string     `json:"page"`
	Pages     []kindJSON `json:"pages"`
	Invert    bool       `json:"invert"`
	Boxes     []string   `json:"boxes"`
	Kinds     []kindJSON `json:"kinds"` // what a Nav box can show
	Light     *lightJSON `json:"light"` // nil: no front light
	Wind      windJSON   `json:"wind"`
	Speed     speedJSON  `json:"speed"`
	Depth     speedJSON  `json:"depth"`
	Demo      bool       `json:"demo"`
	Server    string     `json:"server"`
	Connected bool       `json:"connected"`
	PathKinds []kindJSON `json:"pathKinds,omitempty"` // the paths in use, named
	Limits    limitsJSON `json:"limits"`
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

type limitsJSON struct {
	Boxes int `json:"boxes"`
}

func (s *Server) stateNow() stateJSON {
	c := s.App.Control()
	st := stateJSON{
		Version: s.Version, Platform: s.Platform, Page: c.Page, Invert: c.Invert, Boxes: c.Boxes,
		Wind: windJSON{True: c.WindTrue}, Speed: speedJSON{Kind: c.Speed}, Depth: speedJSON{Kind: c.Depth},
		Demo: c.Demo, Server: c.Server, Connected: c.Connected,
		Limits: limitsJSON{Boxes: pages.NavBoxes},
	}
	for _, p := range c.Pages {
		st.Pages = append(st.Pages, kindJSON{ID: p.ID, Name: p.Name})
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

	if req.Depth != nil {
		if _, ok := pages.DepthFromID(*req.Depth); !ok {
			errs["depth"] = fmt.Sprintf("the depth widget cannot show %q", *req.Depth)
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
	if len(errs) > 0 { // something that passed the checks failed to apply, e.g. the light
		out := s.stateNow()
		writeJSON(w, http.StatusInternalServerError, map[string]any{"errors": errs, "state": out})
		return
	}
	writeJSON(w, http.StatusOK, s.stateNow())
}
