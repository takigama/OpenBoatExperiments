package signalk

import (
	"encoding/json"
	"math"
	"sort"
	"strings"
	"time"
)

// Everything the server sends about our own vessel, as a catalog of numeric
// paths, so any of it can be put on the screen: "anything available in SignalK".
//
// The dashboard only ever shows a few of those, so only the ones asked for -
// "watched" - are copied into each Snapshot (see Own.Watched); the rest are just
// remembered, each as its last value, so that a path that is chosen later (a
// tank level, which may only change every few minutes) shows at once instead of
// waiting for the server to say it again.

// maxCatalog bounds the catalog, since a server can offer hundreds of paths and
// a misbehaving one an endless supply of them.
const maxCatalog = 2000

// catalogEntry is the last value of one path and how often it arrives.
type catalogEntry struct {
	Reading
	gap time.Duration // smoothed time between values; 0 until two have arrived
}

// maxAge is how old the value may get before it should be called stale: a few
// times the usual gap between values, within bounds. A path seen once has no
// known rhythm yet, so it gets the benefit of the doubt for a while.
func (e catalogEntry) maxAge() time.Duration {
	const (
		floor   = 5 * time.Second
		ceiling = 2 * time.Minute
		unknown = 30 * time.Second
	)
	if e.gap == 0 {
		return unknown
	}
	return min(max(4*e.gap, floor), ceiling)
}

// PathReading is a watched path's value, how old it may be and the unit the
// server says it is in, if it said.
type PathReading struct {
	Reading
	MaxAge time.Duration
	Units  string // the server's meta "units" for the path, e.g. "m/s", "rad", "K"; empty if unknown
}

// Fresh reports whether the value is recent enough to show.
func (p PathReading) Fresh(now time.Time) bool { return p.Valid() && now.Sub(p.At) <= p.MaxAge }

// PathInfo is one catalog entry, for listing what is available.
type PathInfo struct {
	Path  string
	Value float64
	At    time.Time
	Units string
}

// noteLocked records the numeric values in a delta for the catalog. An object value
// (a position, an attitude) is recorded field by field, as "path.field".
func (s *State) noteLocked(path string, raw json.RawMessage, now time.Time) {
	if path == "" || isNull(raw) {
		return
	}
	var v float64
	if json.Unmarshal(raw, &v) == nil {
		s.noteNumber(path, v, now)
		return
	}
	var obj map[string]json.RawMessage
	if json.Unmarshal(raw, &obj) != nil {
		return
	}
	for k, rv := range obj {
		var f float64
		if json.Unmarshal(rv, &f) == nil {
			s.noteNumber(path+"."+k, f, now)
		}
	}
}

func (s *State) noteNumber(path string, v float64, now time.Time) {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return
	}
	e, known := s.cat[path]
	if !known && len(s.cat) >= maxCatalog {
		return
	}
	if e.Valid() {
		if d := now.Sub(e.At); d > 0 {
			if e.gap == 0 {
				e.gap = d
			} else {
				e.gap = (7*e.gap + 3*d) / 10
			}
		}
	}
	e.Reading = Reading{V: v, At: now}
	if s.cat == nil {
		s.cat = map[string]catalogEntry{}
	}
	s.cat[path] = e
}

// Watch sets which paths are copied into every Snapshot, returning the ones
// that were not already being watched (so their meta can be fetched).
func (s *State) Watch(paths []string) (added []string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	next := make(map[string]bool, len(paths))
	for _, p := range paths {
		if p == "" {
			continue
		}
		next[p] = true
		if !s.watch[p] {
			added = append(added, p)
		}
	}
	s.watch = next
	sort.Strings(added)
	return added
}

// SetMeta remembers the units the server gives for a path.
func (s *State) SetMeta(path, units string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.meta == nil {
		s.meta = map[string]string{}
	}
	s.meta[path] = units
}

// HasMeta reports whether the units for a path have been asked about already
// (whether or not the server had any to give).
func (s *State) HasMeta(path string) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	_, ok := s.meta[path]
	return ok
}

// Catalog lists every numeric path seen for our own vessel, sorted by name.
func (s *State) Catalog() []PathInfo {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]PathInfo, 0, len(s.cat))
	for p, e := range s.cat {
		out = append(out, PathInfo{Path: p, Value: e.V, At: e.At, Units: s.meta[p]})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out
}

// watchedLocked is the watched paths' readings, for a Snapshot.
func (s *State) watchedLocked() map[string]PathReading {
	if len(s.watch) == 0 {
		return nil
	}
	out := make(map[string]PathReading, len(s.watch))
	for p := range s.watch {
		if e, ok := s.cat[p]; ok {
			out[p] = PathReading{Reading: e.Reading, MaxAge: e.maxAge(), Units: s.meta[p]}
		} else {
			out[p] = PathReading{Units: s.meta[p]}
		}
	}
	return out
}

// Path is a watched path's reading; it is only there for paths that were asked
// for with Watch.
func (o Own) Path(p string) (PathReading, bool) {
	r, ok := o.Watched[p]
	return r, ok
}

// MetaPath is the SignalK REST path of a path's meta, for asking the server what
// units it is in: "environment.wind.speedApparent" becomes
// "environment/wind/speedApparent/meta". Anything but plain path characters is
// refused.
func MetaPath(path string) (string, bool) {
	if path == "" || len(path) > 160 {
		return "", false
	}
	for _, r := range path {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '.', r == '_', r == '-':
		default:
			return "", false
		}
	}
	if strings.HasPrefix(path, ".") || strings.HasSuffix(path, ".") || strings.Contains(path, "..") {
		return "", false
	}
	return strings.ReplaceAll(path, ".", "/") + "/meta", true
}
