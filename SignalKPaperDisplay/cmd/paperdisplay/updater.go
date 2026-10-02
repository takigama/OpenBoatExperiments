package main

import (
	"context"
	"log"
	"os"
	"strconv"
	"time"

	"signalkpaperdisplay/internal/update"
)

// version is the release number, stamped in at build time from the VERSION
// file: go build -ldflags "-X main.version=N". A plain `go build` is v0.
var version = "0"

func versionNumber() int {
	n, _ := strconv.Atoi(version)
	return n
}

func newFetcher(kind, curlBin string) update.Fetcher {
	if kind == "curl" {
		return update.Curl{Bin: curlBin}
	}
	return update.HTTP{}
}

// runUpdate checks for a newer release and, if apply is set, installs it.
// It reports whether a new binary was installed (the caller should then
// exit so the launcher starts the new version).
func runUpdate(ctx context.Context, f update.Fetcher, manifestURL, platform string, apply bool) (bool, error) {
	cur := versionNumber()
	rel, newer, err := update.Check(ctx, f, manifestURL, platform, cur)
	if err != nil {
		return false, err
	}
	if !newer {
		log.Printf("update: up to date (running v%d, newest v%d)", cur, rel.Version)
		return false, nil
	}
	if !apply {
		log.Printf("update: v%d is available (running v%d)", rel.Version, cur)
		return false, nil
	}
	exe, err := os.Executable()
	if err != nil {
		return false, err
	}
	if err := update.Apply(ctx, f, rel, exe); err != nil {
		return false, err
	}
	log.Printf("update: installed v%d (was v%d); previous kept as %s.prev", rel.Version, cur, exe)
	return true, nil
}

// autoUpdate checks every interval and, when it installs something, calls
// restart. The first check waits a little so a boot-time start isn't
// delayed by the network.
func autoUpdate(ctx context.Context, f update.Fetcher, manifestURL, platform string, every time.Duration, restart func()) {
	delay := 30 * time.Second
	for {
		select {
		case <-ctx.Done():
			return
		case <-time.After(delay):
		}
		delay = every
		updated, err := runUpdate(ctx, f, manifestURL, platform, true)
		if err != nil {
			log.Printf("update: %v", err)
			continue
		}
		if updated {
			restart()
			return
		}
	}
}
