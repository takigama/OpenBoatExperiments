package signalk

import (
	"encoding/json"
	"strings"
	"time"
)

// The idle switch. Idle mode follows one SignalK switch, such as
// electrical.switches.bank.0.1.state: off sends the dashboard to sleep, on wakes it.
// A switch's value is a boolean in the specification, but servers and the gadgets
// that feed them send 0 and 1, or "on" and "off", just as often, so all of those
// are understood. It is kept apart from the numeric catalog, which a boolean
// would never reach.

type switchReading struct {
	On    bool
	Known bool
	At    time.Time
}

// SetSwitchPath chooses the path whose on/off value is kept; "" keeps none. What
// was known about the old one is forgotten.
func (s *State) SetSwitchPath(path string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.swPath != path {
		s.swPath = path
		s.sw = switchReading{}
	}
}

// Switch is the idle switch's last value, and when it arrived; known is false until
// the server has said, and again after the server changes.
func (s *State) Switch() (on, known bool, at time.Time) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.sw.On, s.sw.Known, s.sw.At
}

// noteSwitchLocked keeps the value of the idle switch's path. A value that is
// neither a boolean, a number nor a recognisable word changes nothing.
func (s *State) noteSwitchLocked(path string, raw json.RawMessage, now time.Time) {
	if s.swPath == "" || path != s.swPath || isNull(raw) {
		return
	}
	if on, ok := ParseSwitch(raw); ok {
		s.sw = switchReading{On: on, Known: true, At: now}
	}
}

// ParseSwitch reads a switch's JSON value: true/false, a number (non-zero is on), or
// a word such as "on", "off", "true", "false", "1", "0".
func ParseSwitch(raw json.RawMessage) (on, ok bool) {
	var b bool
	if json.Unmarshal(raw, &b) == nil {
		return b, true
	}
	var f float64
	if json.Unmarshal(raw, &f) == nil {
		return f != 0, true
	}
	var str string
	if json.Unmarshal(raw, &str) == nil {
		switch strings.ToLower(strings.TrimSpace(str)) {
		case "on", "true", "1", "yes", "active":
			return true, true
		case "off", "false", "0", "no", "inactive":
			return false, true
		}
	}
	return false, false
}
