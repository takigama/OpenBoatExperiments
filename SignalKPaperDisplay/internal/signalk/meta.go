package signalk

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

// restBase is the server's address for its REST API, worked out from the
// WebSocket address in use: "ws://host:3000/signalk/v1/stream?..." is
// "http://host:3000".
func (c *Client) restBase() (string, error) {
	c.mu.Lock()
	raw := c.url
	if raw == "" {
		raw = c.URL
	}
	c.mu.Unlock()
	u, err := url.Parse(raw)
	if err != nil || u.Host == "" {
		return "", fmt.Errorf("no server address to ask")
	}
	scheme := "http"
	if u.Scheme == "wss" {
		scheme = "https"
	}
	return scheme + "://" + u.Host, nil
}

// FetchMeta asks the server which units one of our own vessel's paths is in (its
// SignalK meta, e.g. "m/s" or "rad"), and remembers the answer in the state. A
// server with no meta for the path is not an error: the units are then empty,
// and remembered as empty so it is not asked again.
func (c *Client) FetchMeta(ctx context.Context, path string) (string, error) {
	mp, ok := MetaPath(path)
	if !ok {
		return "", fmt.Errorf("not a SignalK path: %q", path)
	}
	base, err := c.restBase()
	if err != nil {
		return "", err
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"/signalk/v1/api/vessels/self/"+mp, nil)
	if err != nil {
		return "", err
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	units := ""
	switch {
	case resp.StatusCode == http.StatusNotFound:
	case resp.StatusCode != http.StatusOK:
		return "", fmt.Errorf("meta for %s: %s", path, resp.Status)
	default:
		var m struct {
			Units string `json:"units"`
		}
		if err := json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(&m); err != nil {
			return "", fmt.Errorf("meta for %s: %w", path, err)
		}
		units = m.Units
	}
	c.State.SetMeta(path, units)
	return units, nil
}
