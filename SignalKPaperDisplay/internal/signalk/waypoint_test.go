package signalk

import (
	"testing"
	"time"
)

func TestWaypointDataFromEveryCourseDialect(t *testing.T) {
	cases := map[string]string{
		"v2 course API": "navigation.course.calcValues",
		"great circle":  "navigation.courseGreatCircle.nextPoint",
		"rhumb line":    "navigation.courseRhumbline.nextPoint",
	}
	for name, prefix := range cases {
		st := NewState()
		c := &Client{State: st}
		now := time.Unix(1000, 0)
		c.handle([]byte(hello), now)
		c.handle([]byte(`{"context":"vessels.urn:mrn:signalk:uuid:abc","updates":[{"values":[
			{"path":"`+prefix+`.bearingTrue","value":0.5},
			{"path":"`+prefix+`.distance","value":4200},
			{"path":"`+prefix+`.timeToGo","value":900},
			{"path":"`+prefix+`.velocityMadeGood","value":2.5}
		]}]}`), now)
		o := st.Snapshot().Own
		if o.WPBearing.V != 0.5 || o.WPDistance.V != 4200 || o.WPTimeToGo.V != 900 || o.WPVMG.V != 2.5 {
			t.Errorf("%s: waypoint = %+v %+v %+v %+v", name, o.WPBearing, o.WPDistance, o.WPTimeToGo, o.WPVMG)
		}
	}
}

func TestNoWaypointMeansNoReadings(t *testing.T) {
	st := NewState()
	c := &Client{State: st}
	c.handle([]byte(hello), time.Unix(1000, 0))
	c.handle([]byte(ownDelta), time.Unix(1000, 0))
	o := st.Snapshot().Own
	if o.WPBearing.Valid() || o.WPDistance.Valid() || o.WPTimeToGo.Valid() || o.WPVMG.Valid() {
		t.Error("with no course data none of the waypoint readings may be marked as received")
	}
}
