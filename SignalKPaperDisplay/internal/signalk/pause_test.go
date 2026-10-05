package signalk

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// countingServer counts how many connections it has been given, and how many are open.
func countingServer(t *testing.T) (srv *httptest.Server, conns, open *atomic.Int32) {
	t.Helper()
	conns, open = new(atomic.Int32), new(atomic.Int32)
	up := websocket.Upgrader{}
	srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		conns.Add(1)
		open.Add(1)
		defer open.Add(-1)
		defer conn.Close()
		conn.WriteMessage(websocket.TextMessage, []byte(hello))
		for {
			msg := `{"context":"vessels.self","updates":[{"values":[{"path":"navigation.speedOverGround","value":2}]}]}`
			if conn.WriteMessage(websocket.TextMessage, []byte(msg)) != nil {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
	}))
	t.Cleanup(srv.Close)
	return srv, conns, open
}

func TestPauseDropsTheConnectionAndResumeMakesANewOne(t *testing.T) {
	srv, conns, open := countingServer(t)
	st := NewState()
	c := &Client{URL: StreamURL(strings.TrimPrefix(srv.URL, "http://")), State: st}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)

	waitFor(t, "a connection", func() bool { return st.Snapshot().Connected && st.Snapshot().Own.SOG.Valid() })
	if conns.Load() != 1 {
		t.Fatalf("connections = %d, want 1", conns.Load())
	}

	c.SetPaused(true)
	waitFor(t, "the connection to close", func() bool { return open.Load() == 0 })
	waitFor(t, "the state to say not connected", func() bool { return !st.Snapshot().Connected })
	// And it stays down: no retrying while paused.
	time.Sleep(300 * time.Millisecond)
	if conns.Load() != 1 || open.Load() != 0 {
		t.Errorf("while paused: %d connections made, %d open; want no new ones", conns.Load(), open.Load())
	}
	c.SetPaused(true) // again: nothing
	c.SetPaused(false)
	waitFor(t, "a new connection after the pause", func() bool { return conns.Load() == 2 && st.Snapshot().Connected })
	if open.Load() != 1 {
		t.Errorf("open connections = %d, want 1", open.Load())
	}
	c.SetPaused(false) // again: nothing
	time.Sleep(100 * time.Millisecond)
	if conns.Load() != 2 {
		t.Errorf("resuming twice made another connection: %d", conns.Load())
	}
}

func TestPauseBeforeTheFirstConnectionMakesNone(t *testing.T) {
	srv, conns, _ := countingServer(t)
	c := &Client{URL: StreamURL(strings.TrimPrefix(srv.URL, "http://")), State: NewState()}
	c.SetPaused(true)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)
	time.Sleep(300 * time.Millisecond)
	if conns.Load() != 0 {
		t.Errorf("a paused client connected %d times", conns.Load())
	}
	c.SetPaused(false)
	waitFor(t, "a connection once resumed", func() bool { return conns.Load() == 1 })
}

func TestPausedClientStopsWhenTheContextDoes(t *testing.T) {
	c := &Client{URL: "ws://127.0.0.1:1/signalk/v1/stream", State: NewState()}
	c.SetPaused(true)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { c.Run(ctx); close(done) }()
	time.Sleep(50 * time.Millisecond)
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("Run did not return after its context ended while paused")
	}
}
