package signalk

import (
	"testing"
	"time"
)

func feed(t *testing.T, deltas ...string) Snapshot {
	t.Helper()
	st := NewState()
	c := &Client{State: st}
	now := time.Unix(1000, 0)
	c.handle([]byte(hello), now)
	for _, d := range deltas {
		c.handle([]byte(d), now)
	}
	return st.Snapshot()
}

func ownDeltaOf(values string) string {
	return `{"context":"vessels.urn:mrn:signalk:uuid:abc","updates":[{"values":[` + values + `]}]}`
}

func TestExtraValuesAreKept(t *testing.T) {
	o := feed(t, ownDeltaOf(`
		{"path":"navigation.headingMagnetic","value":1.5},
		{"path":"navigation.rateOfTurn","value":0.01},
		{"path":"steering.rudderAngle","value":-0.1},
		{"path":"environment.outside.pressure","value":101300},
		{"path":"navigation.course.calcValues.crossTrackError","value":-12.5},
		{"path":"electrical.batteries.house.voltage","value":12.6},
		{"path":"electrical.batteries.house.capacity.stateOfCharge","value":0.87},
		{"path":"propulsion.main.revolutions","value":25},
		{"path":"propulsion.main.fuel.rate","value":0.000001},
		{"path":"tanks.freshWater.0.currentLevel","value":0.5}`)).Own
	want := map[string]float64{
		"navigation.headingMagnetic": 1.5, "navigation.rateOfTurn": 0.01, "steering.rudderAngle": -0.1,
		"environment.outside.pressure": 101300, "navigation.course.calcValues.crossTrackError": -12.5,
		"electrical.batteries.house.voltage": 12.6, "electrical.batteries.house.capacity.stateOfCharge": 0.87,
		"propulsion.main.revolutions": 25, "propulsion.main.fuel.rate": 0.000001,
		"tanks.freshWater.0.currentLevel": 0.5,
	}
	for path, v := range want {
		if r := o.ExtraReading(path); !r.Valid() || r.V != v {
			t.Errorf("%s = %+v, want %v", path, r, v)
		}
	}
}

func TestAttitudeObjectAndAutopilotText(t *testing.T) {
	o := feed(t, ownDeltaOf(`
		{"path":"navigation.attitude","value":{"roll":0.1,"pitch":-0.05,"yaw":1.0}},
		{"path":"steering.autopilot.state","value":"auto"}`)).Own
	if r := o.ExtraReading("navigation.attitude.roll"); r.V != 0.1 {
		t.Errorf("roll = %+v", r)
	}
	if r := o.ExtraReading("navigation.attitude.pitch"); r.V != -0.05 {
		t.Errorf("pitch = %+v", r)
	}
	if o.Autopilot.S != "auto" || o.Autopilot.At.IsZero() {
		t.Errorf("autopilot = %+v", o.Autopilot)
	}
}

func TestOnlyWhitelistedExtrasAreKept(t *testing.T) {
	o := feed(t, ownDeltaOf(`
		{"path":"environment.outside.somethingElse","value":1},
		{"path":"electrical.switches.bank.1.state","value":1},
		{"path":"notifications.x","value":2}`)).Own
	if len(o.Extra) != 0 {
		t.Errorf("kept %d paths that nobody asked for: %v", len(o.Extra), o.Extra)
	}
}

func TestNullAndGarbageDoNotMarkAValueSeen(t *testing.T) {
	o := feed(t, ownDeltaOf(`
		{"path":"steering.rudderAngle","value":null},
		{"path":"navigation.rateOfTurn","value":"fast"},
		{"path":"steering.autopilot.state","value":null}`)).Own
	if o.ExtraReading("steering.rudderAngle").Valid() || o.ExtraReading("navigation.rateOfTurn").Valid() {
		t.Error("null or non-numeric values must not count as received")
	}
	if o.Autopilot.S != "" {
		t.Errorf("autopilot = %+v", o.Autopilot)
	}
}

func TestFirstExtraPicksTheLowestId(t *testing.T) {
	o := feed(t, ownDeltaOf(`
		{"path":"electrical.batteries.starter.voltage","value":12.1},
		{"path":"electrical.batteries.house.voltage","value":12.9},
		{"path":"electrical.batteries.house.current","value":-4}`)).Own
	if r := o.FirstExtra("electrical.batteries.", ".voltage"); r.V != 12.9 {
		t.Errorf("first battery voltage = %+v, want house's 12.9 (it sorts before starter)", r)
	}
	if r := o.FirstExtra("electrical.batteries.", ".current"); r.V != -4 {
		t.Errorf("current = %+v", r)
	}
	if o.FirstExtra("propulsion.", ".revolutions").Valid() {
		t.Error("no engine was reported")
	}
}

func TestSnapshotExtraIsACopy(t *testing.T) {
	st := NewState()
	c := &Client{State: st}
	now := time.Unix(1000, 0)
	c.handle([]byte(hello), now)
	c.handle([]byte(ownDeltaOf(`{"path":"steering.rudderAngle","value":0.2}`)), now)
	snap := st.Snapshot()
	c.handle([]byte(ownDeltaOf(`{"path":"steering.rudderAngle","value":0.9}`)), now)
	if snap.Own.ExtraReading("steering.rudderAngle").V != 0.2 {
		t.Error("a snapshot changed underneath its reader")
	}
	st.Reset()
	if len(st.Snapshot().Own.Extra) != 0 || st.Snapshot().Own.Autopilot.S != "" {
		t.Error("Reset must forget the extras")
	}
}
