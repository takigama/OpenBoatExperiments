// Package demo is a stand-in for a SignalK server: a made-up boat sailing about
// with four AIS ships around it, for trying the dashboard with no boat and no
// server. It makes the same delta messages a server would send, and they go
// through the same code that handles a real server's (signalk.State.FeedDemo),
// so nothing downstream knows the difference.
//
// It is the Go version of the throwaway feed the dashboard was developed with,
// with the same rules. All values are SI, as SignalK sends them: metres per
// second, radians, metres, kelvin.
package demo

import (
	"context"
	"encoding/json"
	"math"
	"math/rand"
	"sync"
	"time"

	"signalkpaperdisplay/internal/signalk"
)

const (
	mPerDeg = 111320.0
	nm      = 1852.0
)

func rad(d float64) float64 { return d * math.Pi / 180 }
func deg(r float64) float64 { return r * 180 / math.Pi }

func norm2pi(a float64) float64 {
	a = math.Mod(a, 2*math.Pi)
	if a < 0 {
		a += 2 * math.Pi
	}
	return a
}

func normPi(a float64) float64 {
	a = norm2pi(a)
	if a > math.Pi {
		a -= 2 * math.Pi
	}
	return a
}

type point struct{ lat, lon float64 }

func move(p point, hdg, dist float64) point {
	return point{
		lat: p.lat + dist*math.Cos(hdg)/mPerDeg,
		lon: p.lon + dist*math.Sin(hdg)/(mPerDeg*math.Cos(rad(p.lat))),
	}
}

func pointAt(p point, bearingDeg, rangeM float64) point { return move(p, rad(bearingDeg), rangeM) }

// distBearing is the distance in metres and the bearing in radians from a to b.
func distBearing(a, b point) (dist, bearing float64) {
	dN := (b.lat - a.lat) * mPerDeg
	dE := (b.lon - a.lon) * mPerDeg * math.Cos(rad(a.lat))
	return math.Hypot(dN, dE), norm2pi(math.Atan2(dE, dN))
}

// WindAngles are the wind angles the demo reports. The true wind angle from the
// bow is whatever the true wind direction and our heading make it. The apparent
// wind is that angle pulled toward the bow by between 12 and 28 degrees
// (apparent wind always blows more from ahead than the true wind does),
// wandering slowly - so the two are always at least 10 and never more than 30
// degrees apart, which is what the display's "show the true marker only when it
// differs" rule needs to be exercised.
//
// twdFrom is the true wind direction (where it blows from) and hdg the heading,
// in radians; t is seconds. The results are radians from the bow, positive to
// starboard.
func WindAngles(twdFrom, hdg, t float64) (trueRel, awa, deltaDeg float64) {
	trueRel = normPi(twdFrom - hdg)
	deltaDeg = 20 + 8*math.Sin(t/40) // 12 .. 28
	sign := 1.0
	if trueRel < 0 {
		sign = -1
	}
	awa = normPi(trueRel - sign*rad(deltaDeg)) // toward the bow
	return trueRel, awa, deltaDeg
}

// A ship's standing role relative to us, so the compass and the AIS boxes
// always have each kind to show: two closing (one near, one far) and two
// opening (one near, one far). Courses are re-aimed every tick from where we
// are NOW, so we can wander about and the roles still hold.
type role int

const (
	toward role = iota // steers at us, a few degrees off so it passes rather than collides
	away               // steers directly away from us
)

type target struct {
	mmsi, name string
	role       role
	bearing    float64 // degrees, from our start position
	rangeNm    float64
	offsetDeg  float64
	sog        float64 // m/s
	cog        float64 // degrees
	pos        point
}

// Sim is one run of the demo. It is not safe for concurrent use.
type Sim struct {
	rng      *rand.Rand
	pos      point
	waypoint point
	targets  []*target
}

// start is where the boat begins: off Sydney, where the original feed put it.
var start = point{lat: -33.85, lon: 151.28}

// New makes a run with a fixed random seed, so the same times give the same data.
func New() *Sim {
	s := &Sim{rng: rand.New(rand.NewSource(1)), pos: start}
	// A waypoint 3 nm away at 010 from the start, for the course data.
	s.waypoint = pointAt(start, 10, 3*nm)
	for _, t := range []target{
		{mmsi: "235000001", name: "Triteia", role: toward, bearing: 20, rangeNm: 0.9, offsetDeg: 6, sog: 4},           // closing, near
		{mmsi: "235000002", name: "Aura", role: toward, bearing: 330, rangeNm: 3.5, offsetDeg: -8, sog: 6},            // closing, far
		{mmsi: "235000003", name: "Traviata", role: away, bearing: 190, rangeNm: 1.0, offsetDeg: 10, sog: 5},          // opening, near
		{mmsi: "235000004", name: "To Keel a Sunset", role: away, bearing: 120, rangeNm: 3.0, offsetDeg: -10, sog: 7}, // opening, far
	} {
		t := t
		s.reseed(&t)
		s.targets = append(s.targets, &t)
	}
	return s
}

func (s *Sim) reseed(t *target) { t.pos = pointAt(s.pos, t.bearing, t.rangeNm*nm) }

type value struct {
	Path  string `json:"path"`
	Value any    `json:"value"`
}

func delta(context string, values []value) []byte {
	b, _ := json.Marshal(map[string]any{
		"context": context,
		"updates": []any{map[string]any{
			"source":    map[string]string{"label": "demo", "type": "demo"},
			"timestamp": time.Now().UTC().Format(time.RFC3339),
			"values":    values,
		}},
	})
	return b
}

// Tick moves the boat and the ships on by dt seconds, to t seconds into the run,
// and returns the delta messages a server would send for that moment: one for
// our own vessel and one for each ship.
func (s *Sim) Tick(t, dt float64) [][]byte {
	// Own boat: heading wanders +-25 deg around 045 over ~3 min, speed ~5.4 kn.
	hdg := rad(45 + 25*math.Sin(t/30))
	sog := 2.8 + 0.3*math.Sin(t/17)
	s.pos = move(s.pos, hdg, sog*dt)

	// Wind: true wind from ~090 at ~8 m/s, slowly shifting. The apparent wind
	// speed is derived from the true wind minus the boat's velocity, so it stays
	// physically consistent; its angle is the true angle pulled toward the bow.
	twdFrom := rad(90 + 15*math.Sin(t/90))
	tws := 8 + 2*math.Sin(t/45)
	towardDir := twdFrom + math.Pi
	ae := tws*math.Sin(towardDir) - sog*math.Sin(hdg)
	an := tws*math.Cos(towardDir) - sog*math.Cos(hdg)
	aws := math.Hypot(ae, an)
	_, awa, _ := WindAngles(twdFrom, hdg, t)

	wpDist, wpBearing := distBearing(s.pos, s.waypoint)
	depth := 12 + 6*math.Sin(t/60) + (s.rng.Float64()-0.5)*0.4

	own := []value{
		{"navigation.position", map[string]float64{"latitude": s.pos.lat, "longitude": s.pos.lon}},
		{"navigation.headingTrue", norm2pi(hdg)},
		{"navigation.headingMagnetic", norm2pi(hdg - rad(12))},
		// Leeway/current: the boat goes a few degrees off where it points, and
		// that drifts, so COG and heading visibly differ.
		{"navigation.courseOverGroundTrue", norm2pi(hdg + rad(12)*math.Sin(t/25))},
		{"navigation.speedOverGround", sog},
		{"navigation.speedThroughWater", sog * 0.97},
		{"environment.wind.speedApparent", aws},
		{"environment.wind.angleApparent", awa},
		{"environment.wind.speedTrue", tws},
		{"environment.wind.directionTrue", norm2pi(twdFrom)},
		{"environment.depth.belowTransducer", depth},
		// Next waypoint, as the v2 course API's calcValues report it.
		{"navigation.course.calcValues.bearingTrue", wpBearing},
		{"navigation.course.calcValues.distance", wpDist},
		{"navigation.course.calcValues.timeToGo", wpDist / math.Max(sog, 0.1)},
		// Water temperature (K) and the tanks (ratio 0..1), changing slowly, as
		// a real boat's do.
		{"environment.water.temperature", 297.15 + 1.2*math.Sin(t/120)},
		// The rest of what a Nav box can show: rate of turn, attitude, weather,
		// steering, cross-track error, batteries, an engine, other tanks.
		{"navigation.rateOfTurn", rad(25) * math.Cos(t/30) / 30},
		{"navigation.attitude", map[string]float64{"roll": rad(8 * math.Sin(t/6)), "pitch": rad(2 * math.Sin(t/9)), "yaw": norm2pi(hdg)}},
		{"environment.outside.temperature", 293.15 + 2*math.Sin(t/300)},
		{"environment.outside.pressure", 101300 + 200*math.Sin(t/500)},
		{"environment.outside.humidity", 0.62 + 0.05*math.Sin(t/200)},
		{"steering.rudderAngle", rad(6 * math.Sin(t/12))},
		{"steering.autopilot.state", "auto"},
		{"steering.autopilot.target.headingTrue", norm2pi(hdg + rad(3))},
		{"navigation.course.calcValues.crossTrackError", 40 * math.Sin(t/40)},
		{"electrical.batteries.house.voltage", 12.6 + 0.3*math.Sin(t/60)},
		{"electrical.batteries.house.current", -8 + 3*math.Sin(t/20)},
		{"electrical.batteries.house.capacity.stateOfCharge", math.Max(0.2, 0.9-t/20000)},
		{"electrical.batteries.starter.voltage", 12.9},
		{"propulsion.main.revolutions", 28 + 2*math.Sin(t/10)},
		{"propulsion.main.temperature", 355 + 3*math.Sin(t/50)},
		{"propulsion.main.oilPressure", 300000 + 20000*math.Sin(t/30)},
		{"propulsion.main.fuel.rate", 0.0000014},
		{"tanks.freshWater.0.currentLevel", math.Max(0.05, 0.7-t/40000)},
		{"tanks.wasteWater.0.currentLevel", 0.3},
		{"tanks.blackWater.0.currentLevel", 0.15},
		{"tanks.fuel.0.currentLevel", math.Max(0.05, 0.82-t/20000)},
		{"tanks.fuel.1.currentLevel", math.Max(0.05, 0.43-t/30000)},
	}
	msgs := [][]byte{delta("vessels.self", own)}

	for _, tg := range s.targets {
		// Aim from where we are now: at us (toward) or straight away from us (away).
		_, bearing := distBearing(s.pos, tg.pos) // from us to it
		course := bearing + rad(tg.offsetDeg)
		if tg.role == toward {
			course = bearing + math.Pi + rad(tg.offsetDeg)
		}
		course = norm2pi(course)
		tg.cog = deg(course)
		tg.pos = move(tg.pos, course, tg.sog*dt)
		// Ships that have passed us or wandered off go back to their starting
		// station, so each role is always represented.
		d2, _ := distBearing(s.pos, tg.pos)
		if (tg.role == toward && d2 < 0.12*nm) || (tg.role == away && d2 > 6*nm) {
			s.reseed(tg)
		}
		msgs = append(msgs, delta("vessels.urn:mrn:imo:mmsi:"+tg.mmsi, []value{
			{"", map[string]string{"name": tg.name}},
			{"", map[string]string{"mmsi": tg.mmsi}},
			{"navigation.position", map[string]float64{"latitude": tg.pos.lat, "longitude": tg.pos.lon}},
			{"navigation.courseOverGroundTrue", rad(tg.cog)},
			{"navigation.speedOverGround", tg.sog},
			{"navigation.headingTrue", rad(tg.cog)},
		}))
	}
	return msgs
}

// Interval is how often the demo sends a round of messages, as the original
// feed did.
const Interval = time.Second

// Controller starts and stops the demo feed on a State.
type Controller struct {
	State *signalk.State
	// interval is how often it ticks; zero means Interval. Only the tests set it.
	interval time.Duration

	mu     sync.Mutex
	cancel context.CancelFunc
	done   chan struct{}
}

// Set turns the demo on or off. On, the state forgets whatever it held and
// starts receiving the demo's messages instead of the server's; off, it
// forgets the demo's. Turning it on when it is on, or off when it is off, does
// nothing.
func (c *Controller) Set(on bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if on == (c.cancel != nil) {
		return
	}
	if !on {
		c.cancel()
		<-c.done // no tick still running, so none can arrive after the reset below
		c.cancel, c.done = nil, nil
		c.State.SetDemo(false)
		return
	}
	c.State.SetDemo(true)
	ctx, cancel := context.WithCancel(context.Background())
	c.cancel, c.done = cancel, make(chan struct{})
	go c.run(ctx, c.done)
}

func (c *Controller) run(ctx context.Context, done chan struct{}) {
	defer close(done)
	sim := New()
	began := time.Now()
	last := began
	tick := func(now time.Time) {
		for _, m := range sim.Tick(now.Sub(began).Seconds(), now.Sub(last).Seconds()) {
			c.State.FeedDemo(m, now)
		}
		last = now
	}
	tick(began)
	every := c.interval
	if every <= 0 {
		every = Interval
	}
	timer := time.NewTicker(every)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-timer.C:
			tick(now)
		}
	}
}
