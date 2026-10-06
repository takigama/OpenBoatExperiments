package signalk

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/gorilla/websocket"
)

// A real server says a value when it changes, and once when a connection is made. So
// a switch that begins to be watched while the link is up has to be asked for again,
// which is what Resync is for.
func TestResyncMakesTheServerSayItsStateAgain(t *testing.T) {
	var conns atomic.Int32
	up := websocket.Upgrader{}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		conns.Add(1)
		conn.WriteMessage(websocket.TextMessage, []byte(hello))
		// Everything it knows, once, on connecting - and then nothing more.
		conn.WriteMessage(websocket.TextMessage, deltaOf("navigation.speedOverGround", "3"))
		conn.WriteMessage(websocket.TextMessage, deltaOf("electrical.switches.bank.kindle.0.state", "false"))
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				return
			}
		}
	}))
	defer srv.Close()

	st := NewState()
	c := &Client{URL: StreamURL(strings.TrimPrefix(srv.URL, "http://")), State: st}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)
	waitFor(t, "the first connection's data", func() bool { return st.Snapshot().Own.SOG.Valid() })

	// The switch was said while nobody was watching it: it is not known.
	st.SetSwitchPath("electrical.switches.bank.kindle.0.state")
	if _, known, _ := st.Switch(); known {
		t.Fatal("setup: the switch cannot be known yet")
	}

	sogBefore := st.Snapshot().Own.SOG.At
	c.Resync()
	waitFor(t, "the switch to be said again", func() bool { _, known, _ := st.Switch(); return known })
	if on, _, _ := st.Switch(); on {
		t.Error("the server said false")
	}
	if n := conns.Load(); n != 2 {
		t.Errorf("%d connections, want 2: one at the start and one for the resync", n)
	}
	// Nothing was forgotten on the way: it is a new connection, not a reset.
	waitFor(t, "the speed to arrive afresh", func() bool { return st.Snapshot().Own.SOG.At.After(sogBefore) })

	// With nothing running yet, it does nothing and does not crash.
	(&Client{State: NewState()}).Resync()
}
