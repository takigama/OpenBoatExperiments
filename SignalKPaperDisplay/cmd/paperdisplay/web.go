package main

import "fmt"

// The web page's default port is 80, so the page is at the Kindle's plain address.
// Binding it needs root, which the app has on a Kindle but not on a PC, and
// something else may already be using it: so when 80 is what was asked for (the
// default) and it cannot be had, 8080 is tried instead rather than doing without
// the page.
const (
	defaultWebAddr  = ":80"
	fallbackWebAddr = ":8080"
)

// webAddresses lists the addresses to try, in order, for what -web was given.
// Only ":80" - the default, which a bare -web :80 is the same as - has a fallback:
// any other address that was chosen is the only one tried.
func webAddresses(asked string) []string {
	if asked == defaultWebAddr {
		return []string{defaultWebAddr, fallbackWebAddr}
	}
	return []string{asked}
}

// serveFirst runs serve on each address until one works. serve returns an error at
// once if it cannot bind, and nil once it has served until it was stopped. The
// last error is returned if none works.
func serveFirst(addrs []string, serve func(addr string) error, logf func(format string, args ...any)) error {
	var err error
	for i, addr := range addrs {
		if err = serve(addr); err == nil {
			return nil
		}
		if i+1 < len(addrs) {
			logf("web: cannot serve on %s (%v): trying %s", addr, err, addrs[i+1])
		}
	}
	if err == nil {
		err = fmt.Errorf("no address to serve on")
	}
	return err
}
