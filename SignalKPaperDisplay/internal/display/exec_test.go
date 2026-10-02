package display

import (
	"context"
	"errors"
	"image"
	"image/color"
	"path/filepath"
	"reflect"
	"testing"
)

type call struct {
	bin  string
	args []string
}

func recorder(calls *[]call) func(context.Context, string, ...string) ([]byte, error) {
	return func(_ context.Context, bin string, args ...string) ([]byte, error) {
		*calls = append(*calls, call{bin, args})
		return nil, nil
	}
}

func TestExecSkipsIdenticalFramesButHonoursFullRefresh(t *testing.T) {
	var calls []call
	e := NewEips(4, 4, "eips", filepath.Join(t.TempDir(), "frame.png"))
	e.run = recorder(&calls)
	img := image.NewGray(image.Rect(0, 0, 4, 4))

	var drew []bool
	show := func(full bool) {
		t.Helper()
		d, err := e.Show(img, full)
		if err != nil {
			t.Fatal(err)
		}
		drew = append(drew, d)
	}

	show(false) // first frame always draws
	show(false) // identical -> skipped
	img.Pix[0] = 200
	show(false) // changed -> draws
	show(true)  // identical but forced full refresh -> draws, flashing

	if want := []bool{true, false, true, true}; !reflect.DeepEqual(drew, want) {
		t.Errorf("drew = %v, want %v", drew, want)
	}
	want := []call{
		{"eips", []string{"-g", e.Tmp}},
		{"eips", []string{"-g", e.Tmp}},
		{"eips", []string{"-f", "-g", e.Tmp}},
	}
	if !reflect.DeepEqual(calls, want) {
		t.Errorf("calls = %v, want %v", calls, want)
	}
}

func TestFrameHashIgnoresTheIgnoredRect(t *testing.T) {
	a := image.NewGray(image.Rect(0, 0, 40, 20))
	b := image.NewGray(image.Rect(0, 0, 40, 20))
	ignore := image.Rect(10, 5, 20, 10)

	b.SetGray(12, 7, color.Gray{Y: 200}) // inside the ignored area
	if frameHash(a, ignore) != frameHash(b, ignore) {
		t.Error("a change inside the ignored rect must not change the hash")
	}
	if frameHash(a, image.Rectangle{}) == frameHash(b, image.Rectangle{}) {
		t.Error("without an ignore rect the same change must show up")
	}

	b.SetGray(30, 15, color.Gray{Y: 200}) // outside it
	if frameHash(a, ignore) == frameHash(b, ignore) {
		t.Error("a change outside the ignored rect must change the hash")
	}
	// Edges: the ignored rect touching the image border must not trip up the slicing.
	edge := image.Rect(0, 0, 40, 20)
	if frameHash(a, edge) != frameHash(b, edge) {
		t.Error("ignoring the whole image should hash identically")
	}
}

func TestShowIgnoresChangesInsideTheIgnoredRect(t *testing.T) {
	var calls []call
	e := NewFBInk(40, 20, "fbink", filepath.Join(t.TempDir(), "frame.png"), "DU")
	e.run = recorder(&calls)
	e.IgnoreRect = image.Rect(10, 5, 20, 10)
	img := image.NewGray(image.Rect(0, 0, 40, 20))

	if d, _ := e.Show(img, false); !d {
		t.Fatal("the first frame should draw")
	}
	img.SetGray(12, 7, color.Gray{Y: 255}) // only the heartbeat's area changes
	if d, _ := e.Show(img, false); d {
		t.Error("a frame that differs only in the ignored rect must be skipped")
	}
	img.SetGray(30, 15, color.Gray{Y: 255})
	if d, _ := e.Show(img, false); !d {
		t.Error("a change outside the ignored rect must draw")
	}
}

func TestShowRegion(t *testing.T) {
	var calls []call
	tmp := filepath.Join(t.TempDir(), "frame.png")
	e := NewFBInk(1072, 1448, "fbink", tmp, "")
	e.run = recorder(&calls)
	region := image.NewGray(image.Rect(0, 0, 52, 52))

	if err := e.ShowRegion(region, 800, 20); err != nil {
		t.Fatal(err)
	}
	want := []call{{"fbink", []string{"-q", "-w", "-g", "file=" + tmp + ".region.png,x=800,y=20", "-W", "DU"}}}
	if !reflect.DeepEqual(calls, want) {
		t.Errorf("calls = %v, want %v", calls, want)
	}

	// eips can't, and says so with the sentinel the app looks for.
	if err := NewEips(10, 10, "eips", tmp).ShowRegion(region, 0, 0); !errors.Is(err, ErrNoRegion) {
		t.Errorf("eips ShowRegion = %v, want ErrNoRegion", err)
	}
}

func TestFBInkCommandLines(t *testing.T) {
	const tmp = "/var/tmp/pd.png"
	e := NewFBInk(1072, 1448, "/mnt/us/koreader/fbink", tmp, "GL16")

	if got, want := e.Args(tmp, false), []string{"-q", "-w", "-g", "file=" + tmp, "-W", "GL16"}; !reflect.DeepEqual(got, want) {
		t.Errorf("partial args = %v, want %v", got, want)
	}
	// A full refresh flashes the panel and uses FBInk's own waveform choice.
	if got, want := e.Args(tmp, true), []string{"-q", "-w", "-g", "file=" + tmp, "-f"}; !reflect.DeepEqual(got, want) {
		t.Errorf("full args = %v, want %v", got, want)
	}
	// No waveform configured -> let FBInk pick.
	plain := NewFBInk(1072, 1448, "fbink", tmp, "")
	if got, want := plain.Args(tmp, false), []string{"-q", "-w", "-g", "file=" + tmp}; !reflect.DeepEqual(got, want) {
		t.Errorf("default args = %v, want %v", got, want)
	}
}
