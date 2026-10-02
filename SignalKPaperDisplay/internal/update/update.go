// Package update replaces the running binary with a newer release published
// on GitHub. A small manifest in the repo lists, per platform, the newest
// version, where to download it and its SHA-256. Nothing on disk is touched
// until the download's checksum matches, and the previous binary is kept
// beside the new one (as <exe>.prev) so a bad release can be rolled back.
package update

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"time"
)

// Fetcher downloads a URL into w. Two implementations: Go's own HTTP client
// (fine on a normal Linux host) and curl (for devices like the Kindle,
// whose certificate store Go can't be assumed to find but whose curl is
// known to work against GitHub).
type Fetcher interface {
	Get(ctx context.Context, url string, w io.Writer) error
}

type HTTP struct{ Client *http.Client }

func (h HTTP) Get(ctx context.Context, url string, w io.Writer) error {
	c := h.Client
	if c == nil {
		c = &http.Client{Timeout: 2 * time.Minute}
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	resp, err := c.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("GET %s: %s", url, resp.Status)
	}
	_, err = io.Copy(w, resp.Body)
	return err
}

type Curl struct{ Bin string } // path to curl; empty means "curl" on PATH

func (c Curl) Get(ctx context.Context, url string, w io.Writer) error {
	bin := c.Bin
	if bin == "" {
		bin = "curl"
	}
	var stderr bytes.Buffer
	// -f: fail on HTTP errors instead of saving the error page. -L: GitHub
	// release downloads redirect.
	cmd := exec.CommandContext(ctx, bin, "-fsSL", "--max-time", "180", url)
	cmd.Stdout, cmd.Stderr = w, &stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("curl %s: %w: %s", url, err, strings.TrimSpace(stderr.String()))
	}
	return nil
}

// Release is one downloadable build.
type Release struct {
	Version int    `json:"version"`
	URL     string `json:"url"`
	SHA256  string `json:"sha256"`
}

// Manifest is the file kept in the repo: the newest release per platform.
type Manifest struct {
	Platforms map[string]Release `json:"platforms"`
}

// Check fetches the manifest and reports the newest release for platform,
// and whether it is newer than current.
func Check(ctx context.Context, f Fetcher, manifestURL, platform string, current int) (Release, bool, error) {
	var buf bytes.Buffer
	if err := f.Get(ctx, manifestURL, &buf); err != nil {
		return Release{}, false, err
	}
	var m Manifest
	if err := json.Unmarshal(buf.Bytes(), &m); err != nil {
		return Release{}, false, fmt.Errorf("manifest: %w", err)
	}
	rel, ok := m.Platforms[platform]
	if !ok {
		return Release{}, false, fmt.Errorf("manifest has no entry for platform %q", platform)
	}
	return rel, rel.Version > current, nil
}

// Apply downloads rel and installs it over exe. The old binary is kept as
// exe+".prev". On any failure the running binary is left untouched.
func Apply(ctx context.Context, f Fetcher, rel Release, exe string) error {
	tmp := exe + ".new"
	out, err := os.OpenFile(tmp, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o755)
	if err != nil {
		return err
	}
	h := sha256.New()
	err = f.Get(ctx, rel.URL, io.MultiWriter(out, h))
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	if got := hex.EncodeToString(h.Sum(nil)); !strings.EqualFold(got, rel.SHA256) {
		os.Remove(tmp)
		return fmt.Errorf("checksum mismatch: downloaded %s, manifest says %s", got, rel.SHA256)
	}
	if err := os.Chmod(tmp, 0o755); err != nil {
		os.Remove(tmp)
		return err
	}

	prev := exe + ".prev"
	os.Remove(prev)
	if err := os.Rename(exe, prev); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("keeping previous version: %w", err)
	}
	if err := os.Rename(tmp, exe); err != nil {
		os.Rename(prev, exe) // put the working binary back
		os.Remove(tmp)
		return fmt.Errorf("installing new version: %w", err)
	}
	return nil
}
