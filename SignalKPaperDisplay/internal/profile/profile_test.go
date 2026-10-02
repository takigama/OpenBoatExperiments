package profile

import (
	"os"
	"path/filepath"
	"testing"
)

func TestFrontlightInTheKindleProfile(t *testing.T) {
	p, err := Load(filepath.Join("..", "..", "platforms", "kindle-pw3", "profile.json"))
	if err != nil {
		t.Fatal(err)
	}
	if p.Frontlight != nil && p.Frontlight.Steps > 64 {
		t.Errorf("a setting with %d steps is no use", p.Frontlight.Steps)
	}
	if p.Frontlight == nil || p.Frontlight.Sysfs == "" || p.Frontlight.Lipc == "" || p.Frontlight.LipcMax <= 0 {
		t.Errorf("the Kindle profile should describe its front light, got %+v", p.Frontlight)
	}
}

func TestProfileWithoutALightHasNone(t *testing.T) {
	path := filepath.Join(t.TempDir(), "p.json")
	os.WriteFile(path, []byte(`{"name":"x","width":10,"height":10}`), 0o644)
	p, err := Load(path)
	if err != nil || p.Frontlight != nil {
		t.Errorf("got %+v, %v", p, err)
	}
}
