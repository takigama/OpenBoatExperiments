package main

import (
	"errors"
	"fmt"
	"net"
	"strings"
	"testing"
)

func TestOnlyTheDefaultPortHasAFallback(t *testing.T) {
	if got := webAddresses(":80"); len(got) != 2 || got[0] != ":80" || got[1] != ":8080" {
		t.Errorf("the default tries 80 then 8080, got %v", got)
	}
	for _, chosen := range []string{":8080", ":3000", "127.0.0.1:80", "0.0.0.0:80", ":443"} {
		if got := webAddresses(chosen); len(got) != 1 || got[0] != chosen {
			t.Errorf("%s was chosen: it is the only one to try, got %v", chosen, got)
		}
	}
	if defaultWebAddr != ":80" {
		t.Error("the page is on port 80 unless told otherwise")
	}
}

func TestServeFirstUsesTheFirstAddressThatWorks(t *testing.T) {
	var tried []string
	var logged []string
	logf := func(f string, a ...any) { logged = append(logged, fmt.Sprintf(f, a...)) }

	// The first fails to bind, the second serves.
	err := serveFirst([]string{":80", ":8080"}, func(addr string) error {
		tried = append(tried, addr)
		if addr == ":80" {
			return errors.New("permission denied")
		}
		return nil
	}, logf)
	if err != nil || len(tried) != 2 {
		t.Errorf("err = %v, tried %v", err, tried)
	}
	if len(logged) != 1 || !strings.Contains(logged[0], ":80") || !strings.Contains(logged[0], "permission denied") || !strings.Contains(logged[0], ":8080") {
		t.Errorf("the fallback should say why and where to: %v", logged)
	}

	// The first works: the second is never tried.
	tried, logged = nil, nil
	if err := serveFirst([]string{":80", ":8080"}, func(addr string) error { tried = append(tried, addr); return nil }, logf); err != nil || len(tried) != 1 || len(logged) != 0 {
		t.Errorf("err = %v, tried %v, logged %v", err, tried, logged)
	}

	// None works: the last error comes back.
	err = serveFirst([]string{":80", ":8080"}, func(addr string) error { return errors.New("no " + addr) }, logf)
	if err == nil || err.Error() != "no :8080" {
		t.Errorf("err = %v", err)
	}
	// A single address that fails does not log a fallback.
	logged = nil
	serveFirst([]string{":3000"}, func(string) error { return errors.New("x") }, logf)
	if len(logged) != 0 {
		t.Errorf("there was nothing to fall back to: %v", logged)
	}
	if err := serveFirst(nil, func(string) error { return nil }, logf); err == nil {
		t.Error("with nowhere to serve it should say so")
	}
}

func TestAPortInUseFallsBackForReal(t *testing.T) {
	// Take a port, then ask to serve on it and on a free one.
	busy, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skip("cannot listen here")
	}
	defer busy.Close()
	free, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skip("cannot listen here")
	}
	freeAddr := free.Addr().String()
	free.Close()

	var served string
	err = serveFirst([]string{busy.Addr().String(), freeAddr}, func(addr string) error {
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			return err
		}
		served = addr
		return ln.Close()
	}, func(string, ...any) {})
	if err != nil || served != freeAddr {
		t.Errorf("served on %q (err %v), want the free address %q", served, err, freeAddr)
	}
}
