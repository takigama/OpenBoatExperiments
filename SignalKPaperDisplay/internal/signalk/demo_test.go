package signalk

import (
	"testing"
	"time"
)

const headingDelta = `{"context":"vessels.self","updates":[{"values":[{"path":"navigation.headingTrue","value":1.5}]}]}`

func TestDemoModeSwitchesWhichFeedCounts(t *testing.T) {
	st := NewState()
	c := &Client{State: st}
	now := time.Now()

	c.handle([]byte(headingDelta), now)
	if !st.Snapshot().Own.Heading.Valid() {
		t.Fatal("the server's message should count when not in demo mode")
	}

	// Switching demo mode on forgets what the server sent, and counts as connected.
	st.SetDemo(true)
	snap := st.Snapshot()
	if snap.Own.Heading.Valid() {
		t.Error("the server's data should be gone once demo mode is on")
	}
	if !snap.Connected {
		t.Error("demo mode counts as connected")
	}

	// The server's messages are now ignored, the demo's are not.
	c.handle([]byte(headingDelta), now)
	if st.Snapshot().Own.Heading.Valid() {
		t.Error("the server's message got in during demo mode")
	}
	st.FeedDemo([]byte(headingDelta), now)
	if !st.Snapshot().Own.Heading.Valid() {
		t.Error("the demo's message should count in demo mode")
	}
	if st.Snapshot().LastMessage.IsZero() {
		t.Error("a demo message should stamp the last-message time, or the header says NO DATA")
	}

	// Off again: the demo's data goes, and the server's counts once more.
	st.SetDemo(false)
	snap = st.Snapshot()
	if snap.Own.Heading.Valid() || snap.Connected {
		t.Errorf("demo data survived switching off: %+v", snap)
	}
	c.handle([]byte(headingDelta), now)
	if !st.Snapshot().Own.Heading.Valid() {
		t.Error("the server's message should count again")
	}
	st.SetDemo(false)
	st.FeedDemo([]byte(headingDelta), now)
	if st.Snapshot().Own.Heading.Valid() {
		t.Error("a demo message got in when demo mode was off")
	}
}

func TestSetServerDuringDemoKeepsDemoMode(t *testing.T) {
	st := NewState()
	st.SetDemo(true)
	c := &Client{State: st}
	c.SetServer("10.1.2.3:3000") // resets the state, but is no reason to leave demo mode
	if !st.Demo() {
		t.Error("changing the server must not switch demo mode off")
	}
}

func TestReconnectForgetsEverythingAndAsksAgain(t *testing.T) {
	st := NewState()
	c := &Client{State: st}
	c.handle([]byte(headingDelta), time.Now())
	cancelled := false
	c.mu.Lock()
	c.cancel = func() { cancelled = true }
	c.mu.Unlock()

	c.Reconnect()
	if st.Snapshot().Own.Heading.Valid() {
		t.Error("what was held should be forgotten")
	}
	if !cancelled {
		t.Error("the connection should be dropped, so the server sends everything again")
	}
	if !c.takeChanged() {
		t.Error("Run should reconnect at once, not after a back-off")
	}
}
