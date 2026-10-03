package app

import (
	"os"
	"testing"
	"time"

	"signalkpaperdisplay/internal/pages"
)

func TestDepthWidgetStartsAsDepthAndIsNotSaved(t *testing.T) {
	a, path := controlApp(t)
	if a.Control().Depth != "depth" {
		t.Errorf("the widget should show depth on startup, got %q", a.Control().Depth)
	}
	if err := a.SetDepth("baro"); err != nil {
		t.Fatal(err)
	}
	if a.Control().Depth != "baro" {
		t.Errorf("Depth = %q", a.Control().Depth)
	}
	if !a.takePageChanged() {
		t.Error("changing the widget is a full refresh: its label is a static grey one")
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Error("the widget shows depth on every boot, so nothing is saved for it")
	}
	// Back to depth, by either name.
	for _, id := range []string{"depth", ""} {
		a.SetDepth("baro")
		if err := a.SetDepth(id); err != nil || a.Control().Depth != "depth" {
			t.Errorf("SetDepth(%q): %v, now %q", id, err, a.Control().Depth)
		}
	}
	for _, bad := range []string{"nope", pages.BoxAIS, "path:a b"} {
		if err := a.SetDepth(bad); err == nil {
			t.Errorf("the depth widget cannot show %q", bad)
		}
	}
	if a.Control().Depth != "depth" {
		t.Error("a refused change must change nothing")
	}
}

func TestDepthWidgetPathIsWatchedAndItsUnitsAsked(t *testing.T) {
	a, _ := controlApp(t)
	asked := make(chan string, 4)
	a.FetchMeta = func(p string) { asked <- p }
	if err := a.SetDepth(pages.PathKindID("environment.outside.pressure")); err != nil {
		t.Fatal(err)
	}
	select {
	case p := <-asked:
		if p != "environment.outside.pressure" {
			t.Errorf("asked about %q", p)
		}
	case <-time.After(time.Second):
		t.Fatal("the server was not asked for the units")
	}
	a.State.SetDemo(true)
	a.State.FeedDemo([]byte(`{"context":"vessels.self","updates":[{"values":[{"path":"environment.outside.pressure","value":101300}]}]}`), time.Now())
	if r, ok := a.State.Snapshot().Own.Path("environment.outside.pressure"); !ok || r.V != 101300 {
		t.Errorf("the path should be watched: %+v %v", r, ok)
	}
	// Back to depth: no longer watched.
	a.SetDepth("depth")
	if _, ok := a.State.Snapshot().Own.Path("environment.outside.pressure"); ok {
		t.Error("a path no longer shown should not be watched")
	}
}

func TestTheDepthWidgetIsDrawnWhatItWasSetTo(t *testing.T) {
	a, _ := controlApp(t)
	a.State.SetDemo(true)
	now := time.Now()
	a.State.FeedDemo([]byte(`{"context":"vessels.self","updates":[{"values":[{"path":"environment.depth.belowTransducer","value":12.3},{"path":"navigation.headingTrue","value":1.0},{"path":"navigation.speedOverGround","value":3}]}]}`), now)
	frame := func() []byte {
		img, err := a.Frame(now)
		if err != nil {
			t.Fatal(err)
		}
		return append([]byte(nil), img.Pix...)
	}
	depth := frame()
	a.SetDepth("sog")
	sog := frame()
	same := true
	for i := range depth {
		if depth[i] != sog[i] {
			same = false
			break
		}
	}
	if same {
		t.Error("the frame is the same after setting the depth widget to SOG")
	}
	a.SetDepth("depth")
	back := frame()
	for i := range depth {
		if depth[i] != back[i] {
			t.Fatal("setting it back to depth should draw exactly what it did before")
		}
	}
}
