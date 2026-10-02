package update

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"testing"
)

// fake serves canned bodies by URL.
type fake map[string]string

func (f fake) Get(_ context.Context, url string, w io.Writer) error {
	body, ok := f[url]
	if !ok {
		return fmt.Errorf("404 %s", url)
	}
	_, err := io.WriteString(w, body)
	return err
}

func sum(s string) string {
	h := sha256.Sum256([]byte(s))
	return hex.EncodeToString(h[:])
}

func TestCheck(t *testing.T) {
	f := fake{"m": `{"platforms":{"kindle-pw3":{"version":5,"url":"u","sha256":"x"}}}`}
	ctx := context.Background()

	rel, newer, err := Check(ctx, f, "m", "kindle-pw3", 4)
	if err != nil || !newer || rel.Version != 5 {
		t.Errorf("v4 -> got (%+v, %v, %v), want newer v5", rel, newer, err)
	}
	if _, newer, _ := Check(ctx, f, "m", "kindle-pw3", 5); newer {
		t.Error("same version must not count as an update")
	}
	if _, newer, _ := Check(ctx, f, "m", "kindle-pw3", 9); newer {
		t.Error("running a newer build than the manifest must not downgrade")
	}
	if _, _, err := Check(ctx, f, "m", "kobo-libra", 1); err == nil {
		t.Error("a platform missing from the manifest should be an error")
	}
	if _, _, err := Check(ctx, f, "nope", "kindle-pw3", 1); err == nil {
		t.Error("unreachable manifest should be an error")
	}
	if _, _, err := Check(ctx, fake{"m": "<html>not json"}, "m", "kindle-pw3", 1); err == nil {
		t.Error("garbage manifest should be an error")
	}
}

func TestApplyInstallsAndKeepsPrevious(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "paperdisplay")
	os.WriteFile(exe, []byte("old build"), 0o755)

	newBin := "new build bytes"
	err := Apply(context.Background(), fake{"u": newBin}, Release{Version: 2, URL: "u", SHA256: sum(newBin)}, exe)
	if err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(exe); string(got) != newBin {
		t.Errorf("exe = %q, want the new build", got)
	}
	if got, _ := os.ReadFile(exe + ".prev"); string(got) != "old build" {
		t.Errorf(".prev = %q, want the old build kept for rollback", got)
	}
	if _, err := os.Stat(exe + ".new"); !os.IsNotExist(err) {
		t.Error("the .new staging file should be gone")
	}
}

func TestApplyRefusesBadChecksumAndLeavesExeAlone(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "paperdisplay")
	os.WriteFile(exe, []byte("old build"), 0o755)

	err := Apply(context.Background(), fake{"u": "tampered"}, Release{URL: "u", SHA256: sum("what the manifest promised")}, exe)
	if err == nil {
		t.Fatal("a checksum mismatch must fail")
	}
	if got, _ := os.ReadFile(exe); string(got) != "old build" {
		t.Errorf("exe = %q; a failed update must not touch the running binary", got)
	}
	for _, leftover := range []string{".new", ".prev"} {
		if _, err := os.Stat(exe + leftover); !os.IsNotExist(err) {
			t.Errorf("%s left behind after a failed update", leftover)
		}
	}
}

func TestApplyDownloadFailureLeavesExeAlone(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "paperdisplay")
	os.WriteFile(exe, []byte("old build"), 0o755)

	if err := Apply(context.Background(), fake{}, Release{URL: "missing", SHA256: sum("x")}, exe); err == nil {
		t.Fatal("a failed download must fail")
	}
	if got, _ := os.ReadFile(exe); string(got) != "old build" {
		t.Errorf("exe = %q, want untouched", got)
	}
}
