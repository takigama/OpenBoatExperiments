// Package signalk holds the live data model and a WebSocket client that
// feeds it from a SignalK server. Values are stamped with the *local*
// receive time, not the server's timestamp - the e-ink devices have no
// reliable clock, so server timestamps can't be compared against "now".
package signalk

import (
	"encoding/json"
	"sort"
	"strings"
	"sync"
	"time"
)

// Reading is one numeric value in SignalK's native SI units (m/s, radians,
// metres) plus when it last arrived. The zero value means "never seen".
type Reading struct {
	V  float64
	At time.Time
}

func (r Reading) Valid() bool { return !r.At.IsZero() }

// Fresh reports whether the value arrived within maxAge of now.
func (r Reading) Fresh(now time.Time, maxAge time.Duration) bool {
	return r.Valid() && now.Sub(r.At) <= maxAge
}

type Position struct {
	Lat, Lon float64 // degrees
	At       time.Time
}

func (p Position) Valid() bool { return !p.At.IsZero() }

func (p Position) Fresh(now time.Time, maxAge time.Duration) bool {
	return p.Valid() && now.Sub(p.At) <= maxAge
}

// Tank is one fuel tank. Level is a ratio, 0 (empty) to 1 (full), as
// SignalK reports it.
type Tank struct {
	ID    string // the tank's id in SignalK, e.g. "0" in tanks.fuel.0
	Level Reading
}

// Own is this vessel's data. Angles are radians, speeds m/s, depth metres,
// temperature kelvin.
type Own struct {
	Heading, COG, SOG, STW Reading
	AWA, AWS, TWD, TWS     Reading
	Depth                  Reading
	WaterTemp              Reading
	// The next waypoint, from SignalK's course data: true bearing to it,
	// distance, the server's own time-to-go, and the speed we're making
	// towards it.
	WPBearing, WPDistance, WPTimeToGo, WPVMG Reading
	Fuel                                     []Tank // sorted by ID, so the gauges keep their order
	Pos                                      Position
}

// Target is another vessel (AIS).
type Target struct {
	ID       string // SignalK context id, e.g. "urn:mrn:imo:mmsi:235000001"
	MMSI     string
	Name     string
	Pos      Position
	COG, SOG Reading
}

type Snapshot struct {
	Own         Own
	Targets     []Target // sorted by ID, so output is stable
	Connected   bool
	LastMessage time.Time
}

type State struct {
	mu        sync.RWMutex
	own       Own
	fuel      map[string]Reading // tank id -> level
	targets   map[string]*Target
	connected bool
	lastMsg   time.Time
}

func NewState() *State {
	return &State{targets: map[string]*Target{}, fuel: map[string]Reading{}}
}

func (s *State) Snapshot() Snapshot {
	s.mu.RLock()
	defer s.mu.RUnlock()
	snap := Snapshot{Own: s.own, Connected: s.connected, LastMessage: s.lastMsg}
	for id, lvl := range s.fuel {
		snap.Own.Fuel = append(snap.Own.Fuel, Tank{ID: id, Level: lvl})
	}
	sort.Slice(snap.Own.Fuel, func(i, j int) bool { return snap.Own.Fuel[i].ID < snap.Own.Fuel[j].ID })
	for _, t := range s.targets {
		snap.Targets = append(snap.Targets, *t)
	}
	sort.Slice(snap.Targets, func(i, j int) bool { return snap.Targets[i].ID < snap.Targets[j].ID })
	return snap
}

func (s *State) setConnected(v bool) {
	s.mu.Lock()
	s.connected = v
	s.mu.Unlock()
}

func (s *State) touch(now time.Time) {
	s.mu.Lock()
	s.lastMsg = now
	s.mu.Unlock()
}

func isNull(raw json.RawMessage) bool { return len(raw) == 0 || string(raw) == "null" }

func setNum(r *Reading, raw json.RawMessage, now time.Time) {
	if isNull(raw) {
		return
	}
	var v float64
	if json.Unmarshal(raw, &v) == nil {
		*r = Reading{V: v, At: now}
	}
}

func setPos(p *Position, raw json.RawMessage, now time.Time) {
	if isNull(raw) {
		return
	}
	var v struct{ Latitude, Longitude float64 }
	if json.Unmarshal(raw, &v) == nil {
		*p = Position{Lat: v.Latitude, Lon: v.Longitude, At: now}
	}
}

// fuelTankID pulls the tank id out of a path like tanks.fuel.0.currentLevel.
func fuelTankID(path string) (string, bool) {
	rest, ok := strings.CutPrefix(path, "tanks.fuel.")
	if !ok {
		return "", false
	}
	id, ok := strings.CutSuffix(rest, ".currentLevel")
	return id, ok && id != ""
}

func (s *State) applyOwn(path string, raw json.RawMessage, now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if id, ok := fuelTankID(path); ok {
		r := s.fuel[id]
		setNum(&r, raw, now)
		s.fuel[id] = r
		return
	}
	switch path {
	case "environment.water.temperature":
		setNum(&s.own.WaterTemp, raw, now)
	case "navigation.headingTrue":
		setNum(&s.own.Heading, raw, now)
	case "navigation.courseOverGroundTrue":
		setNum(&s.own.COG, raw, now)
	case "navigation.speedOverGround":
		setNum(&s.own.SOG, raw, now)
	case "navigation.speedThroughWater":
		setNum(&s.own.STW, raw, now)
	case "environment.wind.angleApparent":
		setNum(&s.own.AWA, raw, now)
	case "environment.wind.speedApparent":
		setNum(&s.own.AWS, raw, now)
	case "environment.wind.directionTrue":
		setNum(&s.own.TWD, raw, now)
	case "environment.wind.speedTrue":
		setNum(&s.own.TWS, raw, now)
	case "environment.depth.belowTransducer":
		setNum(&s.own.Depth, raw, now)
	case "navigation.position":
		setPos(&s.own.Pos, raw, now)
	// Waypoint data. The v2 course API publishes it under calcValues; older
	// servers and plugins use courseGreatCircle / courseRhumbline.nextPoint.
	case "navigation.course.calcValues.bearingTrue",
		"navigation.courseGreatCircle.nextPoint.bearingTrue",
		"navigation.courseRhumbline.nextPoint.bearingTrue":
		setNum(&s.own.WPBearing, raw, now)
	case "navigation.course.calcValues.distance",
		"navigation.courseGreatCircle.nextPoint.distance",
		"navigation.courseRhumbline.nextPoint.distance":
		setNum(&s.own.WPDistance, raw, now)
	case "navigation.course.calcValues.timeToGo",
		"navigation.courseGreatCircle.nextPoint.timeToGo",
		"navigation.courseRhumbline.nextPoint.timeToGo":
		setNum(&s.own.WPTimeToGo, raw, now)
	case "navigation.course.calcValues.velocityMadeGood",
		"navigation.courseGreatCircle.nextPoint.velocityMadeGood",
		"navigation.courseRhumbline.nextPoint.velocityMadeGood":
		setNum(&s.own.WPVMG, raw, now)
	}
}

// applyTarget handles a delta for some other vessel. context is the full
// SignalK context, e.g. "vessels.urn:mrn:imo:mmsi:235000001".
func (s *State) applyTarget(context, path string, raw json.RawMessage, now time.Time) {
	id, ok := strings.CutPrefix(context, "vessels.")
	if !ok || id == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.targets[id]
	if t == nil {
		t = &Target{ID: id}
		if _, mmsi, found := strings.Cut(id, "mmsi:"); found {
			t.MMSI = mmsi
		}
		s.targets[id] = t
	}
	switch path {
	case "":
		// Vessel-level object, e.g. {"name":"SEA BREEZE"} or {"mmsi":"..."}.
		var v struct{ Name, MMSI string }
		if !isNull(raw) && json.Unmarshal(raw, &v) == nil {
			if v.Name != "" {
				t.Name = v.Name
			}
			if v.MMSI != "" {
				t.MMSI = v.MMSI
			}
		}
	case "navigation.position":
		setPos(&t.Pos, raw, now)
	case "navigation.courseOverGroundTrue":
		setNum(&t.COG, raw, now)
	case "navigation.speedOverGround":
		setNum(&t.SOG, raw, now)
	}
}
