package signalk

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"time"

	"github.com/gorilla/websocket"
)

// SignalK streams values continuously, so a long silence means the link is
// dead even if the TCP connection still looks open.
const readTimeout = 15 * time.Second

// Client keeps a WebSocket to a SignalK server open and feeds State.
type Client struct {
	URL   string // e.g. ws://host:3000/signalk/v1/stream?subscribe=all
	Token string // optional bearer token for servers with security enabled
	State *State

	self string // the server's id for our own vessel, from its hello message
}

// StreamURL builds the WebSocket URL for a "host:port" server address.
// subscribe=all makes the server include other vessels (AIS), not just us.
func StreamURL(hostPort string) string {
	return "ws://" + hostPort + "/signalk/v1/stream?subscribe=all"
}

// Run connects and reconnects until ctx is cancelled.
func (c *Client) Run(ctx context.Context) {
	backoff := time.Second
	for ctx.Err() == nil {
		started := time.Now()
		err := c.session(ctx)
		c.State.setConnected(false)
		if ctx.Err() != nil {
			return
		}
		if time.Since(started) > 30*time.Second {
			backoff = time.Second // it worked for a while - retry quickly
		}
		log.Printf("signalk: %v (retrying in %s)", err, backoff)
		select {
		case <-ctx.Done():
			return
		case <-time.After(backoff):
		}
		if backoff < 10*time.Second {
			backoff *= 2
		}
	}
}

func (c *Client) session(ctx context.Context) error {
	hdr := http.Header{}
	if c.Token != "" {
		hdr.Set("Authorization", "Bearer "+c.Token)
	}
	dialer := websocket.Dialer{HandshakeTimeout: 5 * time.Second}
	conn, _, err := dialer.DialContext(ctx, c.URL, hdr)
	if err != nil {
		return err
	}
	defer conn.Close()

	stop := make(chan struct{})
	defer close(stop)
	go func() {
		select {
		case <-ctx.Done():
			conn.Close() // unblocks ReadMessage
		case <-stop:
		}
	}()

	c.State.setConnected(true)
	for {
		conn.SetReadDeadline(time.Now().Add(readTimeout))
		_, msg, err := conn.ReadMessage()
		if err != nil {
			return err
		}
		c.handle(msg, time.Now())
	}
}

type message struct {
	Self    string `json:"self"`
	Context string `json:"context"`
	Updates []struct {
		Values []struct {
			Path  string          `json:"path"`
			Value json.RawMessage `json:"value"`
		} `json:"values"`
	} `json:"updates"`
}

// handle applies one WebSocket message (hello or delta) to the state.
func (c *Client) handle(raw []byte, now time.Time) {
	var m message
	if json.Unmarshal(raw, &m) != nil {
		return
	}
	if m.Self != "" {
		c.self = m.Self
	}
	if len(m.Updates) == 0 {
		return
	}
	c.State.touch(now)
	isSelf := m.Context == "" || m.Context == "vessels.self" || m.Context == c.self
	for _, u := range m.Updates {
		for _, v := range u.Values {
			if isSelf {
				c.State.applyOwn(v.Path, v.Value, now)
			} else {
				c.State.applyTarget(m.Context, v.Path, v.Value, now)
			}
		}
	}
}
