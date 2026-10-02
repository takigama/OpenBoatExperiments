package settings

import (
	"path/filepath"
	"testing"
)

func TestNormalizeServer(t *testing.T) {
	good := map[string]string{
		"192.168.1.20:3000": "192.168.1.20:3000",
		"192.168.1.20":      "192.168.1.20:3000", // no port: SignalK's usual
		"  10.0.0.76:3001 ": "10.0.0.76:3001",
		"openplotter.local": "openplotter.local:3000",
		"boat-pi:80":        "boat-pi:80",
		"localhost:3000":    "localhost:3000",
		"255.255.255.255:1": "255.255.255.255:1",
		"10.0.0.1:65535":    "10.0.0.1:65535",
	}
	for in, want := range good {
		if got, err := NormalizeServer(in); err != nil || got != want {
			t.Errorf("NormalizeServer(%q) = %q, %v; want %q", in, got, err, want)
		}
	}
	bad := []string{
		"", "   ", ":3000", "192.168.1", "192.168.1.256", "1.2.3.4.5", "192..1.1", ".1.1.1",
		"192.168.1.20:", "192.168.1.20:0", "192.168.1.20:65536", "192.168.1.20:abc", "10.0.0.1:30:00",
		"has space.local", "bad_host", "1.2.3.4444",
	}
	for _, in := range bad {
		if got, err := NormalizeServer(in); err == nil {
			t.Errorf("NormalizeServer(%q) = %q; want an error", in, got)
		}
	}
}

func TestServerSurvivesTheFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	if err := Save(path, File{Server: "10.0.0.5:3001"}); err != nil {
		t.Fatal(err)
	}
	f, err := Load(path)
	if err != nil || f.Server != "10.0.0.5:3001" || f.Clone().Server != "10.0.0.5:3001" {
		t.Errorf("load = %+v, %v", f, err)
	}
	// Unset stays unset, so the -signalk flag keeps applying.
	if err := Save(path, File{}); err != nil {
		t.Fatal(err)
	}
	if f, _ := Load(path); f.Server != "" {
		t.Errorf("server = %q, want empty", f.Server)
	}
}
