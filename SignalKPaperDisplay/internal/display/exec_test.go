package display

import (
	"context"
	"image"
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
