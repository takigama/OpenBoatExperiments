// Package ais turns SignalK's raw other-vessel data into what a display
// wants: where each ship is relative to us (bearing and range), and whether
// it's closing. It's pure geometry - no I/O, no drawing - so it's tested on
// its own.
package ais

import (
	"math"
	"sort"
	"strings"
	"time"

	"signalkpaperdisplay/internal/signalk"
)

// StaleAfter is how long an AIS target is kept without a new report. Moored
// and anchored ships report only every few minutes, so this is far longer
// than the limit for our own boat's data.
const StaleAfter = 10 * time.Minute

// MaxRange is the furthest a contact is worth showing.
const MaxRange = 12 * 1852.0 // 12 nautical miles, in metres

const metresPerDegree = 111320.0

// Contact is one AIS target as seen from our boat.
type Contact struct {
	ID, Name  string
	Bearing   float64 // true bearing from us to it, radians, 0..2*pi
	Range     float64 // metres
	RangeRate float64 // m/s; negative means the gap is shrinking
	Closing   bool    // RangeRate < 0

	// Closest point of approach, if both keep their present course and speed.
	// Converging is false when we're not getting any nearer (then the other
	// two mean nothing): CPA is the least distance we will be apart, in
	// metres, and TCPA how many seconds from now that will be.
	Converging bool
	CPA, TCPA  float64
}

// DisplayName is what to call the contact on screen: its name if it sent
// one, else its MMSI number, else "UNKNOWN". Many small boats broadcast a
// position but never a name.
func (c Contact) DisplayName() string {
	if n := strings.TrimSpace(c.Name); n != "" {
		return n
	}
	if _, mmsi, ok := strings.Cut(c.ID, "mmsi:"); ok && mmsi != "" {
		return mmsi
	}
	return "UNKNOWN"
}

// MostUrgent is the contact that will pass closest to us - of those that are
// getting nearer at all - or false if none is. Ties go to the sooner one.
func MostUrgent(cs []Contact) (Contact, bool) {
	var best Contact
	found := false
	for _, c := range cs {
		if !c.Converging {
			continue
		}
		if !found || c.CPA < best.CPA || (c.CPA == best.CPA && c.TCPA < best.TCPA) {
			best, found = c, true
		}
	}
	return best, found
}

// velocity is a speed and course as east/north components in m/s.
func velocity(sog, cog signalk.Reading) (e, n float64) {
	if !sog.Valid() || !cog.Valid() {
		return 0, 0 // unknown motion is treated as stationary
	}
	return sog.V * math.Sin(cog.V), sog.V * math.Cos(cog.V)
}

// Contacts returns the targets worth showing, nearest first. ownMaxAge is how
// old our own position and motion may be; without a fresh own position there
// is nothing to measure from, so the result is empty.
func Contacts(own signalk.Own, targets []signalk.Target, now time.Time, ownMaxAge time.Duration) []Contact {
	if !own.Pos.Fresh(now, ownMaxAge) {
		return nil
	}

	// Our motion: course over ground if we have it, else heading.
	course := own.COG
	if !course.Fresh(now, ownMaxAge) {
		course = own.Heading
	}
	speed := own.SOG
	if !speed.Fresh(now, ownMaxAge) {
		speed = signalk.Reading{} // unknown: treat as stationary
	}
	if !course.Fresh(now, ownMaxAge) {
		course = signalk.Reading{}
	}
	oe, on := velocity(speed, course)

	cosLat := math.Cos(own.Pos.Lat * math.Pi / 180)
	var out []Contact
	for _, t := range targets {
		if !t.Pos.Fresh(now, StaleAfter) {
			continue
		}
		// Flat-earth offsets are accurate to well under a percent at the
		// ranges that matter here (a dozen miles).
		de := (t.Pos.Lon - own.Pos.Lon) * metresPerDegree * cosLat
		dn := (t.Pos.Lat - own.Pos.Lat) * metresPerDegree
		dist := math.Hypot(de, dn)
		if dist < 1 || dist > MaxRange {
			continue
		}

		te, tn := 0.0, 0.0
		if t.SOG.Fresh(now, StaleAfter) && t.COG.Fresh(now, StaleAfter) {
			te, tn = velocity(t.SOG, t.COG)
		}
		// Rate of change of the gap: the relative velocity projected onto
		// the line between us.
		rate := ((te-oe)*de + (tn-on)*dn) / dist

		// Closest approach: with relative position r and relative velocity v
		// the gap is smallest after t = -(r.v)/(v.v), if that is ahead of us.
		ve, vn := te-oe, tn-on
		var conv bool
		var cpa, tcpa float64
		if vv := ve*ve + vn*vn; vv > 1e-6 {
			if t := -(de*ve + dn*vn) / vv; t > 0 {
				conv, tcpa = true, t
				cpa = math.Hypot(de+ve*t, dn+vn*t)
			}
		}

		bearing := math.Atan2(de, dn)
		if bearing < 0 {
			bearing += 2 * math.Pi
		}
		out = append(out, Contact{
			ID: t.ID, Name: t.Name,
			Bearing: bearing, Range: dist, RangeRate: rate, Closing: rate < 0,
			Converging: conv, CPA: cpa, TCPA: tcpa,
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Range < out[j].Range })
	return out
}
