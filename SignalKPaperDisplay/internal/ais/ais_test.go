package ais

import (
	"math"
	"testing"
	"time"

	"signalkpaperdisplay/internal/signalk"
)

var now = time.Unix(100000, 0)

func fresh(v float64) signalk.Reading { return signalk.Reading{V: v, At: now} }

// ownAt is a boat at the given spot, heading/moving as given.
func ownAt(lat, lon, sog, cog float64) signalk.Own {
	return signalk.Own{
		Pos: signalk.Position{Lat: lat, Lon: lon, At: now},
		SOG: fresh(sog), COG: fresh(cog), Heading: fresh(cog),
	}
}

// target placed dNorth/dEast metres from (lat, lon).
func target(id string, lat, lon, dNorth, dEast, sog, cog float64) signalk.Target {
	cosLat := math.Cos(lat * math.Pi / 180)
	return signalk.Target{
		ID: id, Name: id,
		Pos: signalk.Position{
			Lat: lat + dNorth/metresPerDegree,
			Lon: lon + dEast/(metresPerDegree*cosLat),
			At:  now,
		},
		SOG: fresh(sog), COG: fresh(cog),
	}
}

const lat, lon = -33.85, 151.28

func near(a, b, tol float64) bool { return math.Abs(a-b) <= tol }

func TestBearingAndRange(t *testing.T) {
	own := ownAt(lat, lon, 0, 0)
	cases := []struct {
		name           string
		dN, dE         float64
		wantBearingDeg float64
	}{
		{"north", 1852, 0, 0},
		{"east", 0, 1852, 90},
		{"south", -1852, 0, 180},
		{"west", 0, -1852, 270},
		{"north-east", 1000, 1000, 45},
	}
	for _, c := range cases {
		got := Contacts(own, []signalk.Target{target(c.name, lat, lon, c.dN, c.dE, 0, 0)}, now, 10*time.Second)
		if len(got) != 1 {
			t.Fatalf("%s: got %d contacts, want 1", c.name, len(got))
		}
		wantRange := math.Hypot(c.dN, c.dE)
		if !near(got[0].Range, wantRange, wantRange*0.005) {
			t.Errorf("%s: range = %.1f, want %.1f", c.name, got[0].Range, wantRange)
		}
		if deg := got[0].Bearing * 180 / math.Pi; !near(deg, c.wantBearingDeg, 0.5) {
			t.Errorf("%s: bearing = %.2f deg, want %.0f", c.name, deg, c.wantBearingDeg)
		}
	}
}

func TestClosingVersusOpening(t *testing.T) {
	// A stationary ship dead ahead (north), while we move north then south.
	ship := target("ship", lat, lon, 2000, 0, 0, 0)

	toward := Contacts(ownAt(lat, lon, 5, 0), []signalk.Target{ship}, now, 10*time.Second)
	if !toward[0].Closing || !near(toward[0].RangeRate, -5, 0.01) {
		t.Errorf("heading at it at 5 m/s: closing=%v rate=%.2f, want closing at -5", toward[0].Closing, toward[0].RangeRate)
	}
	away := Contacts(ownAt(lat, lon, 5, math.Pi), []signalk.Target{ship}, now, 10*time.Second)
	if away[0].Closing || !near(away[0].RangeRate, 5, 0.01) {
		t.Errorf("heading away: closing=%v rate=%.2f, want opening at +5", away[0].Closing, away[0].RangeRate)
	}

	// Their motion counts too: a ship ahead, steaming south toward us at
	// 8 m/s, while we sit still.
	coming := target("coming", lat, lon, 2000, 0, 8, math.Pi)
	got := Contacts(ownAt(lat, lon, 0, 0), []signalk.Target{coming}, now, 10*time.Second)
	if !got[0].Closing || !near(got[0].RangeRate, -8, 0.01) {
		t.Errorf("ship approaching while we're stopped: %+v", got[0])
	}

	// Passing abeam at the same speed: the gap isn't changing.
	abeam := target("abeam", lat, lon, 0, 2000, 5, 0)
	got = Contacts(ownAt(lat, lon, 5, 0), []signalk.Target{abeam}, now, 10*time.Second)
	if !near(got[0].RangeRate, 0, 0.01) {
		t.Errorf("same velocity abeam: rate = %.3f, want 0", got[0].RangeRate)
	}
}

func TestFiltering(t *testing.T) {
	own := ownAt(lat, lon, 0, 0)

	if got := Contacts(own, []signalk.Target{target("far", lat, lon, MaxRange+500, 0, 0, 0)}, now, 10*time.Second); len(got) != 0 {
		t.Error("a target beyond MaxRange should be dropped")
	}
	if got := Contacts(own, []signalk.Target{target("here", lat, lon, 0, 0, 0, 0)}, now, 10*time.Second); len(got) != 0 {
		t.Error("a target at our own position has no bearing and should be dropped")
	}

	old := target("old", lat, lon, 1000, 0, 0, 0)
	old.Pos.At = now.Add(-StaleAfter - time.Minute)
	if got := Contacts(own, []signalk.Target{old}, now, 10*time.Second); len(got) != 0 {
		t.Error("a target silent for longer than StaleAfter should be dropped")
	}
	// ...but a ship that reported 5 minutes ago is fine: moored ships report slowly.
	slow := target("moored", lat, lon, 1000, 0, 0, 0)
	slow.Pos.At = now.Add(-5 * time.Minute)
	if got := Contacts(own, []signalk.Target{slow}, now, 10*time.Second); len(got) != 1 {
		t.Error("a target that reported 5 minutes ago should still be shown")
	}

	// Without a fresh position of our own, bearings would be meaningless.
	blind := own
	blind.Pos.At = now.Add(-time.Minute)
	if got := Contacts(blind, []signalk.Target{target("x", lat, lon, 1000, 0, 0, 0)}, now, 10*time.Second); got != nil {
		t.Error("with no fresh own position there must be no contacts")
	}
}

func TestSortedNearestFirst(t *testing.T) {
	own := ownAt(lat, lon, 0, 0)
	got := Contacts(own, []signalk.Target{
		target("far", lat, lon, 5000, 0, 0, 0),
		target("near", lat, lon, 0, 800, 0, 0),
		target("mid", lat, lon, -2500, 0, 0, 0),
	}, now, 10*time.Second)
	if len(got) != 3 || got[0].ID != "near" || got[1].ID != "mid" || got[2].ID != "far" {
		t.Errorf("order = %+v, want near, mid, far", got)
	}
}

func TestUnknownMotionIsTreatedAsStationary(t *testing.T) {
	// A target that reports a position but no course/speed (many Class B
	// units): it still appears, as a stationary contact.
	tg := target("quiet", lat, lon, 1500, 0, 0, 0)
	tg.SOG, tg.COG = signalk.Reading{}, signalk.Reading{}
	got := Contacts(ownAt(lat, lon, 4, 0), []signalk.Target{tg}, now, 10*time.Second)
	if len(got) != 1 || !got[0].Closing {
		t.Errorf("got %+v, want one contact, closing only because we're moving toward it", got)
	}
}
