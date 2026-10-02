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
