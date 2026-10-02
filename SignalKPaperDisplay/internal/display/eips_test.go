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

	show := func(full bool) {
		t.Helper()
		if err := e.Show(img, full); err != nil {
			t.Fatal(err)
		}
	}

	show(false) // first frame always draws
	show(false) // identical -> skipped
	img.Pix[0] = 200
	show(false) // changed -> draws
	show(true)  // identical but forced full refresh -> draws, flashing

	want := [][]string{
		{"-g", e.Tmp},
		{"-g", e.Tmp},
		{"-f", "-g", e.Tmp},
	}
	if !reflect.DeepEqual(calls, want) {
		t.Errorf("eips calls = %v, want %v", calls, want)
	}
}
