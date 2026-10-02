package signalk

import (
	"testing"
	"time"
)

const hello = `{"name":"signalk-server","version":"2.33.0","self":"vessels.urn:mrn:signalk:uuid:abc"}`

const ownDelta = `{"context":"vessels.urn:mrn:signalk:uuid:abc","updates":[{"values":[
	{"path":"navigation.speedOverGround","value":2.9},
	{"path":"navigation.headingTrue","value":0.7854},
	{"path":"environment.depth.belowTransducer","value":12.5},
	{"path":"navigation.position","value":{"latitude":-33.85,"longitude":151.28}},
	{"path":"navigation.speedThroughWater","value":null}
]}]}`

const aisDelta = `{"context":"vessels.urn:mrn:imo:mmsi:235000001","updates":[{"values":[
	{"path":"","value":{"name":"SEA BREEZE"}},
	{"path":"navigation.position","value":{"latitude":-33.82,"longitude":151.30}},
	{"path":"navigation.courseOverGroundTrue","value":3.49}
]}]}`

func TestHandleSplitsOwnAndTargets(t *testing.T) {
	st := NewState()
	c := &Client{State: st}
	now := time.Unix(1000, 0)

	c.handle([]byte(hello), now)
	c.handle([]byte(ownDelta), now)
	c.handle([]byte(aisDelta), now)

	snap := st.Snapshot()
	if got := snap.Own.SOG.V; got != 2.9 {
		t.Errorf("own SOG = %v, want 2.9", got)
	}
	if got := snap.Own.Depth.V; got != 12.5 {
		t.Errorf("own depth = %v, want 12.5", got)
	}
	if !snap.Own.Pos.Valid() || snap.Own.Pos.Lat != -33.85 {
		t.Errorf("own position = %+v", snap.Own.Pos)
	}
	if snap.Own.STW.Valid() {
		t.Errorf("a null value must not mark STW as received")
	}
	if len(snap.Targets) != 1 {
		t.Fatalf("targets = %d, want 1", len(snap.Targets))
	}
	tg := snap.Targets[0]
	if tg.Name != "SEA BREEZE" || tg.MMSI != "235000001" || tg.COG.V != 3.49 {
		t.Errorf("target = %+v", tg)
	}
}

func TestWaterTempAndFuelTanks(t *testing.T) {
	st := NewState()
	c := &Client{State: st}
	now := time.Unix(1000, 0)
	c.handle([]byte(hello), now)
	c.handle([]byte(`{"context":"vessels.urn:mrn:signalk:uuid:abc","updates":[{"values":[
		{"path":"environment.water.temperature","value":297.15},
		{"path":"tanks.fuel.1.currentLevel","value":0.45},
		{"path":"tanks.fuel.0.currentLevel","value":0.82},
		{"path":"tanks.fuel.0.name","value":"Main"},
		{"path":"tanks.freshWater.0.currentLevel","value":0.9}
	]}]}`), now)

	own := st.Snapshot().Own
	if own.WaterTemp.V != 297.15 {
		t.Errorf("water temperature = %v, want 297.15 K", own.WaterTemp.V)
	}
	if len(own.Fuel) != 2 {
		t.Fatalf("fuel tanks = %+v, want exactly the two fuel tanks (not fresh water)", own.Fuel)
	}
	// Sorted by id, whatever order the server sent them in.
	if own.Fuel[0].ID != "0" || own.Fuel[0].Level.V != 0.82 || own.Fuel[1].ID != "1" || own.Fuel[1].Level.V != 0.45 {
		t.Errorf("fuel = %+v, want tank 0 at 0.82 then tank 1 at 0.45", own.Fuel)
	}

	// A later update to one tank changes only that tank.
	later := now.Add(time.Minute)
	c.handle([]byte(`{"context":"vessels.urn:mrn:signalk:uuid:abc","updates":[{"values":[
		{"path":"tanks.fuel.1.currentLevel","value":0.40}]}]}`), later)
	own = st.Snapshot().Own
	if own.Fuel[1].Level.V != 0.40 || !own.Fuel[1].Level.At.Equal(later) || own.Fuel[0].Level.V != 0.82 {
		t.Errorf("fuel after update = %+v", own.Fuel)
	}
}

func TestFreshness(t *testing.T) {
	now := time.Unix(1000, 0)
	r := Reading{V: 1, At: now.Add(-3 * time.Second)}
	if !r.Fresh(now, 5*time.Second) {
		t.Error("3s-old reading should be fresh within 5s")
	}
	if r.Fresh(now, 2*time.Second) {
		t.Error("3s-old reading should be stale within 2s")
	}
	if (Reading{}).Fresh(now, time.Hour) {
		t.Error("a reading that never arrived must never be fresh")
	}
}
