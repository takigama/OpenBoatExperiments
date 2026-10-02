package display

import (
	"context"
	"image"
	"path/filepath"
	"reflect"
	"testing"
)

func TestEipsSkipsIdenticalFramesButHonoursFullRefresh(t *testing.T) {
	var calls [][]string
	e := &Eips{
		W: 4, H: 4, Bin: "eips",
		Tmp: filepath.Join(t.TempDir(), "frame.png"),
		runEips: func(_ context.Context, args ...string) ([]byte, error) {
			calls = append(calls, args)
			return nil, nil
		},
	}
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

	want := [][]string{
		{"-g", e.Tmp},
		{"-g", e.Tmp},
		{"-f", "-g", e.Tmp},
	}
	if !reflect.DeepEqual(calls, want) {
		t.Errorf("eips calls = %v, want %v", calls, want)
	}
}
