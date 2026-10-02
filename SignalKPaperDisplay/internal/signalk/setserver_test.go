package signalk

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// fakeServer is a SignalK server that streams the given speed once a moment.
func fakeServer(t *testing.T, sog string) *httptest.Server {
	t.Helper()
	up := websocket.Upgrader{}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		conn.WriteMessage(websocket.TextMessage, []byte(hello))
		for {
			msg := `{"context":"vessels.self","updates":[{"values":[{"path":"navigation.speedOverGround","value":` + sog + `}]}]}`
			if conn.WriteMessage(websocket.TextMessage, []byte(msg)) != nil {
				return
			}
			time.Sleep(20 * time.Millisecond)
		}
	}))
	t.Cleanup(srv.Close)
	return srv
}

func waitFor(t *testing.T, what string, ok func() bool) {
	t.Helper()
	for i := 0; i < 200; i++ {
		if ok() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestSetServerMovesTheConnectionAndForgetsTheOldData(t *testing.T) {
	a, b := fakeServer(t, "1.5"), fakeServer(t, "2.5")
	host := func(s *httptest.Server) string { return strings.TrimPrefix(s.URL, "http://") }

	st := NewState()
	c := &Client{URL: StreamURL(host(a)), State: st}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)

	waitFor(t, "data from the first server", func() bool { return st.Snapshot().Own.SOG.V == 1.5 })

	// Something only the first server knew: it must not outlive the switch.
	st.applyTarget("vessels.urn:mrn:imo:mmsi:235000001", "navigation.speedOverGround", []byte("3"), time.Now())
	if len(st.Snapshot().Targets) != 1 {
		t.Fatal("test setup: expected a target")
	}

	c.SetServer(host(b))
	waitFor(t, "data from the second server", func() bool { return st.Snapshot().Own.SOG.V == 2.5 })
	if n := len(st.Snapshot().Targets); n != 0 {
		t.Errorf("%d targets from the old server survived the switch", n)
	}
	if !st.Snapshot().Connected {
		t.Error("should be connected to the new server")
	}
}

func TestSetServerBeforeRunIsUsed(t *testing.T) {
	b := fakeServer(t, "4.5")
	st := NewState()
	c := &Client{URL: "ws://127.0.0.1:1/never", State: st}
	c.SetServer(strings.TrimPrefix(b.URL, "http://"))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)
	waitFor(t, "data from the server set before Run", func() bool { return st.Snapshot().Own.SOG.V == 4.5 })
}

func TestStateReset(t *testing.T) {
	st := NewState()
	now := time.Now()
	st.applyOwn("navigation.speedOverGround", []byte("2"), now)
	st.applyOwn("tanks.fuel.0.currentLevel", []byte("0.5"), now)
	st.setConnected(true)
	st.touch(now)
	st.Reset()
	s := st.Snapshot()
	if s.Own.SOG.Valid() || len(s.Own.Fuel) != 0 || s.Connected || !s.LastMessage.IsZero() {
		t.Errorf("after Reset: %+v", s)
	}
}
