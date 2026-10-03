package demo

import (
	"math"
	"testing"
	"time"

	"signalkpaperdisplay/internal/signalk"
)

// feed runs the sim for n one-second ticks into a state that is in demo mode,
// returning the sim so a test can look inside it.
func feed(t *testing.T, st *signalk.State, n int) *Sim {
	t.Helper()
	st.SetDemo(true)
	sim := New()
	now := time.Now()
	for i := 0; i < n; i++ {
		for _, m := range sim.Tick(float64(i), 1) {
			st.FeedDemo(m, now)
		}
	}
	return sim
}

func TestWindAnglesStayBetweenTwelveAndTwentyEightDegreesApart(t *testing.T) {
	// Whatever way the boat points and however the wind shifts: the apparent
	// angle is the true one pulled 12-28 degrees toward the bow.
	for hdg := 0.0; hdg < 360; hdg += 7 {
		for twd := 0.0; twd < 360; twd += 11 {
			for sec := 0.0; sec < 600; sec += 13 {
				trueRel, awa, delta := WindAngles(rad(twd), rad(hdg), sec)
				if delta < 12-1e-9 || delta > 28+1e-9 {
					t.Fatalf("delta %.2f outside 12..28 (hdg %v twd %v t %v)", delta, hdg, twd, sec)
				}
				if got := math.Abs(deg(normPi(trueRel - awa))); math.Abs(got-delta) > 1e-6 {
					t.Fatalf("true %.1f and apparent %.1f are %.2f apart, want %.2f", deg(trueRel), deg(awa), got, delta)
				}
				// Pulled toward the bow, never away from it (unless it crosses it,
				// which still leaves the apparent wind nearer ahead).
				if math.Abs(awa) > math.Abs(trueRel)+1e-9 && math.Abs(trueRel) > rad(28) {
					t.Fatalf("apparent %.1f is further from the bow than true %.1f", deg(awa), deg(trueRel))
				}
			}
		}
	}
}

func TestWindAnglesBehaveOnEachSide(t *testing.T) {
	// Wind on the starboard bow (true 60): the apparent is nearer the bow.
	tr, awa, d := WindAngles(rad(60), 0, 0)
	if math.Abs(deg(tr)-60) > 1e-6 || math.Abs(deg(awa)-(60-d)) > 1e-6 {
		t.Errorf("starboard: true %.1f apparent %.1f", deg(tr), deg(awa))
	}
	// On the port bow (true -60): mirrored.
	tr, awa, d = WindAngles(rad(300), 0, 0)
	if math.Abs(deg(tr)+60) > 1e-6 || math.Abs(deg(awa)+(60-d)) > 1e-6 {
		t.Errorf("port: true %.1f apparent %.1f", deg(tr), deg(awa))
	}
}

func TestOneTickGivesEverythingTheDashboardShows(t *testing.T) {
	st := signalk.NewState()
	feed(t, st, 1)
	snap := st.Snapshot()
	own := snap.Own
	for name, r := range map[string]signalk.Reading{
		"heading": own.Heading, "cog": own.COG, "sog": own.SOG, "stw": own.STW,
		"awa": own.AWA, "aws": own.AWS, "twd": own.TWD, "tws": own.TWS,
		"depth": own.Depth, "water temperature": own.WaterTemp,
		"waypoint bearing": own.WPBearing, "waypoint distance": own.WPDistance, "waypoint time to go": own.WPTimeToGo,
	} {
		if !r.Valid() {
			t.Errorf("no %s", name)
		}
	}
	if !own.Pos.Valid() {
		t.Error("no position")
	}
	if len(own.Fuel) != 2 {
		t.Errorf("fuel tanks = %d, want 2", len(own.Fuel))
	}
	if own.Autopilot.S != "auto" {
		t.Errorf("autopilot = %q", own.Autopilot.S)
	}
	for _, path := range []string{
		"navigation.headingMagnetic", "navigation.rateOfTurn", "navigation.attitude.roll", "navigation.attitude.pitch",
		"environment.outside.temperature", "environment.outside.pressure", "environment.outside.humidity",
		"steering.rudderAngle", "steering.autopilot.target.headingTrue", "navigation.course.calcValues.crossTrackError",
		"electrical.batteries.house.voltage", "electrical.batteries.house.current", "electrical.batteries.house.capacity.stateOfCharge",
		"electrical.batteries.starter.voltage", "propulsion.main.revolutions", "propulsion.main.temperature",
		"propulsion.main.oilPressure", "propulsion.main.fuel.rate", "tanks.freshWater.0.currentLevel",
		"tanks.wasteWater.0.currentLevel", "tanks.blackWater.0.currentLevel",
	} {
		if !own.ExtraReading(path).Valid() {
			t.Errorf("no %s", path)
		}
	}
	if len(snap.Targets) != 4 {
		t.Fatalf("targets = %d, want 4", len(snap.Targets))
	}
	for _, tg := range snap.Targets {
		if tg.Name == "" || tg.MMSI == "" || !tg.Pos.Valid() || !tg.COG.Valid() || !tg.SOG.Valid() {
			t.Errorf("target incomplete: %+v", tg)
		}
	}
	if !snap.Connected || snap.LastMessage.IsZero() {
		t.Errorf("a demo should count as connected with data arriving: %+v", snap)
	}
}

func TestValuesAreInSIUnitsAndPlausible(t *testing.T) {
	st := signalk.NewState()
	feed(t, st, 600)
	own := st.Snapshot().Own
	rng := func(name string, v, lo, hi float64) {
		if v < lo || v > hi {
			t.Errorf("%s = %v, want %v..%v", name, v, lo, hi)
		}
	}
	rng("heading rad", own.Heading.V, 0, 2*math.Pi)
	rng("sog m/s", own.SOG.V, 2, 4)
	rng("depth m", own.Depth.V, 5, 19)
	rng("true wind speed m/s", own.TWS.V, 5, 11)
	rng("water temp K", own.WaterTemp.V, 290, 300)
	rng("lat", own.Pos.Lat, -34, -33)
	rng("lon", own.Pos.Lon, 151, 152)
	rng("waypoint distance m", own.WPDistance.V, 0, 10000)
	for _, k := range []string{"tanks.freshWater.0.currentLevel", "tanks.wasteWater.0.currentLevel"} {
		rng(k, own.ExtraReading(k).V, 0, 1)
	}
	for _, tk := range own.Fuel {
		rng("fuel "+tk.ID, tk.Level.V, 0, 1)
	}
}

func TestEachShipKeepsItsRole(t *testing.T) {
	// Two ships always close on us and two always open, wherever we wander; a
	// ship that has passed us or gone far off starts again from its station.
	sim := New()
	prev := make([]float64, len(sim.targets))
	for i, tg := range sim.targets {
		prev[i], _ = distBearing(sim.pos, tg.pos)
	}
	reseeds := 0
	for i := 0; i < 2400; i++ {
		sim.Tick(float64(i), 1)
		for j, tg := range sim.targets {
			d, _ := distBearing(sim.pos, tg.pos)
			jumped := math.Abs(d-prev[j]) > 500
			switch {
			case jumped:
				reseeds++
			case tg.role == toward && d >= prev[j]:
				t.Fatalf("t=%d: %s should be closing but went from %.0f m to %.0f m", i, tg.name, prev[j], d)
			case tg.role == away && d <= prev[j]:
				t.Fatalf("t=%d: %s should be opening but went from %.0f m to %.0f m", i, tg.name, prev[j], d)
			}
			prev[j] = d
		}
	}
	if reseeds == 0 {
		t.Error("in 40 minutes no ship ever passed or left, so the restart rule was never exercised")
	}
}

func TestSameRunGivesSameData(t *testing.T) {
	a, b := New(), New()
	for i := 0; i < 100; i++ {
		a.Tick(float64(i), 1)
		b.Tick(float64(i), 1)
	}
	if a.pos != b.pos || a.targets[0].pos != b.targets[0].pos {
		t.Errorf("two runs differ: %+v vs %+v", a.pos, b.pos)
	}
}

func TestFeedDemoIsIgnoredWhenNotInDemoMode(t *testing.T) {
	st := signalk.NewState()
	for _, m := range New().Tick(0, 1) {
		st.FeedDemo(m, time.Now())
	}
	if snap := st.Snapshot(); snap.Own.Heading.Valid() || len(snap.Targets) != 0 || snap.Connected {
		t.Errorf("demo data reached a state that was not in demo mode: %+v", snap)
	}
}

func waitFor(t *testing.T, what string, ok func() bool) {
	t.Helper()
	for i := 0; i < 200; i++ {
		if ok() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestControllerStartsAndStopsTheFeed(t *testing.T) {
	st := signalk.NewState()
	c := &Controller{State: st, interval: 10 * time.Millisecond}

	c.Set(true)
	if !st.Demo() {
		t.Fatal("Set(true) should put the state in demo mode")
	}
	waitFor(t, "data", func() bool { return st.Snapshot().Own.Heading.Valid() })
	c.Set(true) // again: nothing

	// It keeps going: heading changes as time passes.
	h0 := st.Snapshot().Own.Pos
	waitFor(t, "the boat to move", func() bool { return st.Snapshot().Own.Pos != h0 })

	c.Set(false)
	if st.Demo() {
		t.Fatal("Set(false) should end demo mode")
	}
	if snap := st.Snapshot(); snap.Own.Heading.Valid() || len(snap.Targets) != 0 || snap.Connected {
		t.Fatalf("demo data survived switching it off: %+v", snap)
	}
	time.Sleep(60 * time.Millisecond)
	if st.Snapshot().Own.Heading.Valid() {
		t.Fatal("the feed went on after it was switched off")
	}
	c.Set(false) // again: nothing

	// And it can be switched on again, from the start.
	c.Set(true)
	waitFor(t, "data again", func() bool { return st.Snapshot().Own.Heading.Valid() })
	c.Set(false)
}
