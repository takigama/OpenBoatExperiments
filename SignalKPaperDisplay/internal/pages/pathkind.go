package pages

import (
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"
	"unicode"

	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// A box (or the compass speed widget) can show any numeric path the SignalK
// server sends, not only the built-in kinds: its kind ID is "path:" and the path.
// How to write the number depends on what it measures, which comes from the
// units the server gives for the path (its SignalK meta) when it gives any, and
// else from what the path is called.

// PathPrefix starts the ID of a box kind that shows a raw SignalK path.
const PathPrefix = "path:"

// IsPathKind reports whether a kind ID is a raw SignalK path.
func IsPathKind(id string) bool { return strings.HasPrefix(id, PathPrefix) }

// PathOf is the SignalK path of a path kind ID.
func PathOf(id string) string { return strings.TrimPrefix(id, PathPrefix) }

// PathKindID is the kind ID for a SignalK path.
func PathKindID(path string) string { return PathPrefix + path }

// ValidPathKind reports whether id is a well-formed path kind: plain characters,
// no empty segments. Whether the server really has the path is another matter.
func ValidPathKind(id string) bool {
	if !IsPathKind(id) {
		return false
	}
	_, ok := signalk.MetaPath(PathOf(id))
	return ok
}

// WatchedPaths is the SignalK paths a set of kind IDs need watching.
func WatchedPaths(ids ...string) []string {
	var out []string
	seen := map[string]bool{}
	for _, id := range ids {
		if IsPathKind(id) && !seen[id] {
			seen[id] = true
			out = append(out, PathOf(id))
		}
	}
	return out
}

// genericRoots are the SignalK top-level groups, which say little in a label.
var genericRoots = map[string]bool{
	"navigation": true, "environment": true, "electrical": true, "propulsion": true,
	"tanks": true, "steering": true, "performance": true, "sensors": true, "design": true,
}

// PathLabel is a short name for a path, for the label over its value: the last
// three parts (without the top-level group), the camel case split into words, in
// capitals. "environment.wind.speedApparent" is "WIND SPEED APPARENT".
func PathLabel(path string) string {
	parts := strings.Split(path, ".")
	if len(parts) > 1 && genericRoots[parts[0]] {
		parts = parts[1:]
	}
	if len(parts) > 3 {
		parts = parts[len(parts)-3:]
	}
	var words []string
	for _, p := range parts {
		var cur []rune
		for i, r := range p {
			if i > 0 && unicode.IsUpper(r) && !unicode.IsUpper(rune(p[i-1])) {
				words = append(words, string(cur))
				cur = nil
			}
			cur = append(cur, r)
		}
		words = append(words, string(cur))
	}
	return short(strings.Join(words, " "), 32)
}

// pathKind is the BoxKind of a path kind ID.
func pathKind(id string) BoxKind {
	return BoxKind{ID: id, Name: PathOf(id), Label: PathLabel(PathOf(id))}
}

type pathClass int

const (
	classNumber   pathClass = iota // no idea: just the number
	classSpeed                     // m/s
	classLength                    // m, as a depth or height
	classDistance                  // m, as a distance
	classAngleAbs                  // rad, a direction: 000..359
	classAngleRel                  // rad, relative to the bow: with stbd or port
	classAngle                     // rad, any other angle: signed degrees
	classRate                      // rad/s, as degrees a minute
	classTemp                      // K
	classPressure                  // Pa
	classRatio                     // 0..1, as a percentage
	classVolts
	classAmps
	classWatts
	classHertz
	classRevs     // revolutions per second, as rpm
	classDuration // seconds
	classFlow     // m3/s, as litres an hour
)

// classFromUnits is what the server's units for a path say it is, or false.
func classFromUnits(u string) (pathClass, bool) {
	switch strings.ToLower(strings.TrimSpace(u)) {
	case "m/s":
		return classSpeed, true
	case "rad":
		return classAngle, true
	case "rad/s":
		return classRate, true
	case "k":
		return classTemp, true
	case "pa":
		return classPressure, true
	case "m":
		return classLength, true
	case "ratio":
		return classRatio, true
	case "v":
		return classVolts, true
	case "a":
		return classAmps, true
	case "w":
		return classWatts, true
	case "hz":
		return classHertz, true
	case "s":
		return classDuration, true
	case "m3/s":
		return classFlow, true
	}
	return classNumber, false
}

// classFromName guesses what a path measures from what it is called, for
// servers that give no units. Names follow the SignalK specification, so this is
// right for the standard paths; a path it does not know is shown as a plain
// number. The last part of the name is read first ("speedApparent"), then the
// whole of it ("environment.depth.belowKeel").
func classFromName(path string) pathClass {
	p := strings.ToLower(path)
	last := p[strings.LastIndex(p, ".")+1:]
	if c := classify(last, p); c != classNumber {
		return c
	}
	return classify(p, p)
}

func classify(name, whole string) pathClass {
	has := func(subs ...string) bool {
		for _, s := range subs {
			if strings.Contains(name, s) {
				return true
			}
		}
		return false
	}
	switch {
	case has("currentlevel", "stateofcharge", "humidity", "level"):
		return classRatio
	case has("rateofturn"):
		return classRate
	case has("speed", "velocity"):
		return classSpeed
	case has("heading", "bearing", "direction", "course", "yaw"):
		return classAngleAbs
	case has("angle"):
		return classAngleRel
	case has("pitch", "roll", "trim"):
		return classAngle
	case has("temperature"):
		return classTemp
	case has("pressure"):
		return classPressure
	case has("depth", "draft", "airgap", "height", "elevation", "altitude"):
		return classLength
	case has("distance", "range", "crosstrack"):
		return classDistance
	case has("voltage"):
		return classVolts
	case has("current"):
		return classAmps
	case has("revolutions"):
		return classRevs
	case has("frequency"):
		return classHertz
	case has("power"):
		return classWatts
	case has("timetogo", "duration"):
		return classDuration
	case strings.HasSuffix(whole, "fuel.rate"):
		return classFlow
	}
	return classNumber
}

// plain writes a number with about four significant digits and no exponent.
func plain(v float64) string {
	switch a := math.Abs(v); {
	case a >= 1000:
		return strconv.FormatFloat(v, 'f', 0, 64)
	case a >= 10:
		return strconv.FormatFloat(v, 'f', 1, 64)
	case a >= 1:
		return strconv.FormatFloat(v, 'f', 2, 64)
	case a == 0:
		return "0"
	default:
		return strconv.FormatFloat(v, 'f', 3, 64)
	}
}

// FormatPath writes the value of a SignalK path in the user's units: the value
// and its unit. units is what the server gave for it, "" if it gave none.
func FormatPath(path, serverUnits string, v float64, u units.Settings) (value, unit string) {
	class, known := classFromUnits(serverUnits)
	byName := classFromName(path)
	switch {
	case !known:
		class = byName
	// The units alone leave out how to say it: metres may be a depth or a
	// distance, a radian a bearing or an angle off the bow. The name decides.
	case class == classLength && (byName == classLength || byName == classDistance):
		class = byName
	case class == classAngle && (byName == classAngleAbs || byName == classAngleRel || byName == classAngle):
		class = byName
	}
	switch class {
	case classSpeed:
		return u.Format("sog", v)
	case classLength:
		return u.Format("depth", v)
	case classDistance:
		return u.Format("range", v)
	case classAngleAbs:
		a := math.Mod(v*180/math.Pi, 360)
		if a < 0 {
			a += 360
		}
		return fmt.Sprintf("%03.0f", a), "°"
	case classAngleRel:
		return sideAngle(v, 0)
	case classAngle:
		return fmt.Sprintf("%+.1f", v*180/math.Pi), "°"
	case classRate:
		return fmt.Sprintf("%+.0f", v*180/math.Pi*60), "°/min"
	case classTemp:
		return u.Format("watertemp", v)
	case classPressure:
		if p := strings.ToLower(path); strings.HasPrefix(p, "propulsion.") || strings.Contains(p, "oil") || strings.Contains(p, "coolant") || strings.Contains(p, "boost") {
			// An engine's pressures are small ones: bar, or psi under the imperial preset.
			if u.Preset == units.PresetImperial {
				return fmt.Sprintf("%.0f", v*0.000145038), "psi"
			}
			return fmt.Sprintf("%.1f", v*0.00001), "bar"
		}
		return u.FormatQuantity(units.Pressure, v)
	case classRatio:
		return percent(v), "%"
	case classVolts:
		return fmt.Sprintf("%.1f", v), "V"
	case classAmps:
		return fmt.Sprintf("%+.1f", v), "A"
	case classWatts:
		return plain(v), "W"
	case classHertz:
		return plain(v), "Hz"
	case classRevs:
		return fmt.Sprintf("%.0f", v*60), "rpm"
	case classDuration:
		return formatDuration(v)
	case classFlow:
		if u.Preset == units.PresetImperial {
			return fmt.Sprintf("%.1f", v*3.6e6*0.264172), "gal/h"
		}
		return fmt.Sprintf("%.1f", v*3.6e6), "L/h"
	}
	return plain(v), ""
}

// pathMetric is a box showing a raw SignalK path.
func pathMetric(id string, own signalk.Own, now time.Time, e Env) metric {
	k := pathKind(id)
	m := metric{label: k.Label}
	if r, ok := own.Path(PathOf(id)); ok {
		m.value, m.unit = FormatPath(PathOf(id), r.Units, r.V, e.Units)
		m.ok = r.Fresh(now)
	}
	return m
}
