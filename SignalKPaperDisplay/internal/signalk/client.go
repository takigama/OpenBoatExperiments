package signalk

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"sync"
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

	mu      sync.Mutex
	url     string             // the address in use, once SetServer has changed it
	cancel  context.CancelFunc // ends the current connection
	changed bool               // the server was changed since the last connection ended
}

// SetServer points the client at a different "host:port" and reconnects to it
// at once. Everything learned from the old server is forgotten. It's safe to
// call while Run is running.
func (c *Client) SetServer(hostPort string) {
	c.mu.Lock()
	c.url = StreamURL(hostPort)
	c.changed = true
	cancel := c.cancel
	c.mu.Unlock()
	c.State.Reset()
	if cancel != nil {
		cancel() // the dropped connection makes Run reconnect, to the new address
	}
}

// Reconnect drops the connection and makes a new one to the same server, which
// sends its whole state again. Everything held is forgotten first. It's for
// when the demo ends: while it ran the server's messages were ignored, so
// values that only change rarely (a tank level) would otherwise stay missing
// until they next changed.
func (c *Client) Reconnect() {
	c.mu.Lock()
	c.changed = true
	cancel := c.cancel
	c.mu.Unlock()
	c.State.Reset()
	if cancel != nil {
		cancel()
	}
}

// takeChanged reports, and clears, whether the server was changed.
func (c *Client) takeChanged() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	ch := c.changed
	c.changed = false
	return ch
}

// StreamURL builds the WebSocket URL for a "host:port" server address.
// subscribe=all makes the server include other vessels (AIS), not just us.
func StreamURL(hostPort string) string {
	return "ws://" + hostPort + "/signalk/v1/stream?subscribe=all"
}

// retryLog decides which failures to log. A server that is simply down fails
// the same way every few seconds, which would fill the log for as long as it
// stays down: the first occurrence and any change are logged, then only one
// in sixty of a repeat.
type retryLog struct {
	last string
	n    int
}

func (r *retryLog) should(err error) bool {
	if msg := err.Error(); msg != r.last {
		r.last, r.n = msg, 0
		return true
	}
	r.n++
	return r.n%60 == 0
}

// Run connects and reconnects until ctx is cancelled.
func (c *Client) Run(ctx context.Context) {
	backoff := time.Second
	var rl retryLog
	for ctx.Err() == nil {
		started := time.Now()
		err := c.session(ctx)
		c.State.setConnected(false)
		if ctx.Err() != nil {
			return
		}
		if c.takeChanged() {
			log.Printf("signalk: reconnecting")
			backoff = time.Second
			continue // no waiting: the new address hasn't failed yet
		}
		if time.Since(started) > 30*time.Second {
			backoff = time.Second // it worked for a while - retry quickly
			rl = retryLog{}       // and the next failure is news again
		}
		if rl.should(err) {
			log.Printf("signalk: %v (retrying in %s)", err, backoff)
		}
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

func (c *Client) session(parent context.Context) error {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	// The address and the means of ending this connection are taken together,
	// so a SetServer that comes after this point is sure to end it.
	c.mu.Lock()
	url := c.url
	if url == "" {
		url = c.URL
	}
	c.cancel = cancel
	c.mu.Unlock()

	hdr := http.Header{}
	if c.Token != "" {
		hdr.Set("Authorization", "Bearer "+c.Token)
	}
	dialer := websocket.Dialer{HandshakeTimeout: 5 * time.Second}
	conn, _, err := dialer.DialContext(ctx, url, hdr)
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

// handle applies one WebSocket message (hello or delta) to the state, unless
// the demo is standing in for the server.
func (c *Client) handle(raw []byte, now time.Time) {
	var m message
	if json.Unmarshal(raw, &m) != nil {
		return
	}
	if m.Self != "" {
		c.self = m.Self
	}
	if c.State.Demo() {
		return
	}
	c.State.apply(m, c.self, now)
}
