package signalk

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestParseSwitch(t *testing.T) {
	for _, tc := range []struct {
		raw      string
		on, good bool
	}{
		{`true`, true, true}, {`false`, false, true},
		{`1`, true, true}, {`0`, false, true}, {`0.0`, false, true}, {`2.5`, true, true},
		{`"on"`, true, true}, {`"ON"`, true, true}, {`"off"`, false, true}, {`" Off "`, false, true},
		{`"true"`, true, true}, {`"false"`, false, true}, {`"1"`, true, true}, {`"0"`, false, true},
		{`"banana"`, false, false}, {`{"state":1}`, false, false}, {`[1]`, false, false},
	} {
		on, ok := ParseSwitch(json.RawMessage(tc.raw))
		if on != tc.on || ok != tc.good {
			t.Errorf("%s = (on %v, ok %v), want (%v, %v)", tc.raw, on, ok, tc.on, tc.good)
		}
	}
}

func deltaOf(path, value string) []byte {
	return []byte(`{"context":"vessels.self","updates":[{"values":[{"path":"` + path + `","value":` + value + `}]}]}`)
}

func mustMessage(t *testing.T, raw []byte) message {
	t.Helper()
	var m message
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatal(err)
	}
	return m
}

func TestSwitchIsKeptOnlyForItsPath(t *testing.T) {
	s := NewState()
	feed := func(path, v string) { s.apply(mustMessage(t, deltaOf(path, v)), "", time.Now()) }
	feed("electrical.switches.kindle.state", "false")
	if _, known, _ := s.Switch(); known {
		t.Fatal("no switch was chosen: nothing should be kept")
	}

	s.SetSwitchPath("electrical.switches.kindle.state")
	feed("electrical.switches.other.state", "true")
	if _, known, _ := s.Switch(); known {
		t.Error("another switch's value was taken for this one")
	}
	feed("electrical.switches.kindle.state", "true")
	if on, known, at := s.Switch(); !on || !known || at.IsZero() {
		t.Errorf("after true: on=%v known=%v", on, known)
	}
	feed("electrical.switches.kindle.state", "0")
	if on, known, _ := s.Switch(); on || !known {
		t.Errorf("after 0: on=%v known=%v", on, known)
	}
	// A value that means nothing leaves what was known alone.
	feed("electrical.switches.kindle.state", `"banana"`)
	feed("electrical.switches.kindle.state", `null`)
	if on, known, _ := s.Switch(); on || !known {
		t.Errorf("junk changed the switch: on=%v known=%v", on, known)
	}

	// A different server knows nothing yet - but the path to watch stays.
	s.Reset()
	if _, known, _ := s.Switch(); known {
		t.Error("the old server's switch survived a reset")
	}
	feed("electrical.switches.kindle.state", "true")
	if on, known, _ := s.Switch(); !on || !known {
		t.Errorf("after a reset the path should still be watched: on=%v known=%v", on, known)
	}
	// Choosing another path forgets the value; the same path does not.
	s.SetSwitchPath("electrical.switches.kindle.state")
	if _, known, _ := s.Switch(); !known {
		t.Error("choosing the same path again forgot its value")
	}
	s.SetSwitchPath("electrical.switches.nav.state")
	if _, known, _ := s.Switch(); known {
		t.Error("a different path should start unknown")
	}
}

// recorder is a server that notes how each connection was made and what the client
// said, and answers a subscription to its switch with a value.
type recorder struct {
	mu    sync.Mutex
	query []string // each connection's query string
	said  []string // everything clients sent, in order
	conns int
}

func (r *recorder) handler(switchValue string) http.Handler {
	up := websocket.Upgrader{}
	return http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		conn, err := up.Upgrade(w, req, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		r.mu.Lock()
		r.conns++
		r.query = append(r.query, req.URL.RawQuery)
		r.mu.Unlock()
		conn.WriteMessage(websocket.TextMessage, []byte(hello))
		if strings.Contains(req.URL.RawQuery, "subscribe=all") {
			conn.WriteMessage(websocket.TextMessage, deltaOf("navigation.speedOverGround", "3"))
		}
		for { // read: this is also what answers the client's pings
			_, msg, err := conn.ReadMessage()
			if err != nil {
				return
			}
			r.mu.Lock()
			r.said = append(r.said, string(msg))
			r.mu.Unlock()
			if strings.Contains(string(msg), "electrical.switches.kindle.state") {
				conn.WriteMessage(websocket.TextMessage, deltaOf("electrical.switches.kindle.state", switchValue))
			}
		}
	})
}

func (r *recorder) snapshot() (conns int, query, said []string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.conns, append([]string(nil), r.query...), append([]string(nil), r.said...)
}

func TestIdleNarrowsTheSubscriptionToTheSwitchAndWidensItAgain(t *testing.T) {
	rec := &recorder{}
	srv := httptest.NewServer(rec.handler("false"))
	defer srv.Close()
	st := NewState()
	st.SetSwitchPath("electrical.switches.kindle.state")
	c := &Client{URL: StreamURL(strings.TrimPrefix(srv.URL, "http://")), State: st}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)

	waitFor(t, "everything to arrive", func() bool { return st.Snapshot().Own.SOG.Valid() })

	c.SetIdle("electrical.switches.kindle.state")
	waitFor(t, "the switch's value over the narrow connection", func() bool {
		_, known, _ := st.Switch()
		return known
	})
	conns, query, said := rec.snapshot()
	if conns != 2 || !strings.Contains(query[0], "subscribe=all") || !strings.Contains(query[1], "subscribe=none") {
		t.Fatalf("connections %d, queries %v: want one for everything, then one for nothing", conns, query)
	}
	if len(said) != 1 || !strings.Contains(said[0], `"path":"electrical.switches.kindle.state"`) || !strings.Contains(said[0], `"context":"vessels.self"`) {
		t.Errorf("the client said %v, want one subscription to the switch", said)
	}
	if on, _, _ := st.Switch(); on {
		t.Error("the switch said false")
	}

	// While narrow, nothing else comes: what was learned stays, but it is not refreshed.
	before := st.Snapshot().Own.SOG.At
	time.Sleep(150 * time.Millisecond)
	if !st.Snapshot().Own.SOG.At.Equal(before) {
		t.Error("speed kept arriving on the narrow connection")
	}

	c.SetIdle("electrical.switches.kindle.state") // the same again: nothing
	time.Sleep(100 * time.Millisecond)
	if n, _, _ := rec.snapshot(); n != 2 {
		t.Errorf("asking for the same idle path again made a connection (%d)", n)
	}

	c.SetIdle("")
	waitFor(t, "everything again", func() bool { return st.Snapshot().Own.SOG.At.After(before) })
	if n, q, _ := rec.snapshot(); n != 3 || !strings.Contains(q[2], "subscribe=all") {
		t.Errorf("connections %d, queries %v: want a third one, for everything", n, q)
	}
	if _, known, _ := st.Switch(); !known {
		t.Error("the switch's last value was lost on the way back")
	}
}

func TestIdleConnectionPingsSoQuietIsNotMistakenForDead(t *testing.T) {
	oldPing, oldRead := idlePing, idleReadTimeout
	idlePing, idleReadTimeout = 40*time.Millisecond, 300*time.Millisecond
	defer func() { idlePing, idleReadTimeout = oldPing, oldRead }()

	rec := &recorder{}
	srv := httptest.NewServer(rec.handler("true"))
	defer srv.Close()
	st := NewState()
	st.SetSwitchPath("electrical.switches.kindle.state")
	c := &Client{URL: StreamURL(strings.TrimPrefix(srv.URL, "http://")), State: st}
	c.SetIdle("electrical.switches.kindle.state")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)

	waitFor(t, "the narrow connection", func() bool { _, known, _ := st.Switch(); return known })
	time.Sleep(1200 * time.Millisecond) // four times the read timeout, with nothing sent but pings
	if n, _, _ := rec.snapshot(); n != 1 {
		t.Errorf("%d connections: a quiet link was dropped although its pings were answered", n)
	}
}

func TestIdleConnectionIsDroppedWhenNothingAnswers(t *testing.T) {
	oldPing, oldRead := idlePing, idleReadTimeout
	idlePing, idleReadTimeout = 40*time.Millisecond, 200*time.Millisecond
	defer func() { idlePing, idleReadTimeout = oldPing, oldRead }()

	var mu sync.Mutex
	conns := 0
	up := websocket.Upgrader{}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		mu.Lock()
		conns++
		mu.Unlock()
		time.Sleep(2 * time.Second) // never reads, so never answers a ping
	}))
	defer srv.Close()
	c := &Client{URL: StreamURL(strings.TrimPrefix(srv.URL, "http://")), State: NewState()}
	c.SetIdle("electrical.switches.kindle.state")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)
	waitFor(t, "a reconnection", func() bool {
		mu.Lock()
		defer mu.Unlock()
		return conns >= 2
	})
}
