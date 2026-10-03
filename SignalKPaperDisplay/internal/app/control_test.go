package app

import (
	"image"
	"os"
	"path/filepath"
	"testing"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/frontlight"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func controlApp(t *testing.T) (*App, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "settings.json")
	a := &App{
		State:   signalk.NewState(),
		Display: &display.PNG{W: 1072, H: 1448, Path: filepath.Join(t.TempDir(), "frame.png")},
		Units:   units.Settings{Preset: units.PresetMetric}, SettingsPath: path,
	}
	a.SetPage("compass")
	a.takePageChanged()
	return a, path
}

func saved(t *testing.T, path string) settings.File {
	t.Helper()
	f, err := settings.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func TestControlReportsTheState(t *testing.T) {
	a, _ := controlApp(t)
	a.Light = frontlight.NewFake(24, 7)
	a.InitLight()
	c := a.Control()
	if c.Page != "compass" || c.Invert || c.WindTrue || c.Speed != "sog" || c.LightMax != 24 || c.Light != 7 {
		t.Errorf("initial state: %+v", c)
	}
	if len(c.Boxes) != pages.NavBoxes || c.Boxes[0] != pages.DefaultBoxes()[0] {
		t.Errorf("boxes: %v", c.Boxes)
	}
	if len(c.Pages) != len(pages.All()) || c.Pages[0].ID == "" || c.Pages[0].Name == "" {
		t.Errorf("pages: %+v", c.Pages)
	}
	// No light on this device: reported as none.
	b, _ := controlApp(t)
	if got := b.Control(); got.LightMax != 0 || got.Light != 0 {
		t.Errorf("a device with no light: %+v", got)
	}
}

func TestChoosePage(t *testing.T) {
	a, _ := controlApp(t)
	if err := a.ChoosePage("nav"); err != nil {
		t.Fatal(err)
	}
	if a.Control().Page != "nav" || !a.takePageChanged() {
		t.Error("a new page is shown with a full refresh")
	}
	if err := a.ChoosePage("nope"); err == nil {
		t.Error("an unknown page should be refused")
	}
	if a.Control().Page != "nav" {
		t.Error("a refused page must leave the page alone")
	}
}

func TestSetInvertSavesAndRedrawsEverything(t *testing.T) {
	a, path := controlApp(t)
	a.SetInvert(true)
	if !a.Control().Invert || !saved(t, path).Invert {
		t.Error("invert should be on, and saved")
	}
	if !a.takePageChanged() {
		t.Error("inverting changes every pixel: it should be a full refresh")
	}
	img, err := a.Frame(time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if dark := img.GrayAt(5, 600).Y; dark > 40 {
		t.Errorf("the page is not inverted: background pixel is %d", dark)
	}
	a.SetInvert(false)
	if a.Control().Invert || saved(t, path).Invert {
		t.Error("invert should be off again, and saved")
	}
}

func TestSetBoxChangesOneBoxAndSaves(t *testing.T) {
	a, path := controlApp(t)
	if err := a.SetBox(2, "baro"); err != nil {
		t.Fatal(err)
	}
	want := pages.DefaultBoxes()
	want[2] = "baro"
	got := a.Control().Boxes
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("boxes = %v, want %v", got, want)
		}
	}
	if f := saved(t, path); len(f.Boxes) != pages.NavBoxes || f.Boxes[2] != "baro" {
		t.Errorf("saved boxes: %v", f.Boxes)
	}
	if !a.takePageChanged() {
		t.Error("changing a box is a full refresh, as it is from the touch screen")
	}
	for _, tc := range []struct {
		i    int
		kind string
	}{{-1, "sog"}, {pages.NavBoxes, "sog"}, {0, "nope"}, {0, "path:a b"}, {0, ""}} {
		if err := a.SetBox(tc.i, tc.kind); err == nil {
			t.Errorf("SetBox(%d, %q) should be refused", tc.i, tc.kind)
		}
	}
	if a.Control().Boxes[0] != pages.DefaultBoxes()[0] {
		t.Error("a refused change must change nothing")
	}
	// The closest-AIS box is one a Nav box can show.
	if err := a.SetBox(0, pages.BoxAIS); err != nil {
		t.Error(err)
	}
}

func TestAPathInABoxIsWatchedAndItsUnitsAsked(t *testing.T) {
	a, path := controlApp(t)
	asked := make(chan string, 4)
	a.FetchMeta = func(p string) { asked <- p }
	now := time.Now()
	c := &signalk.Client{State: a.State}
	_ = c

	id := pages.PathKindID("vendor.custom.thing")
	if err := a.SetBox(1, id); err != nil {
		t.Fatal(err)
	}
	select {
	case p := <-asked:
		if p != "vendor.custom.thing" {
			t.Errorf("asked about %q", p)
		}
	case <-time.After(time.Second):
		t.Fatal("the server was not asked what units the path is in")
	}
	// The value arriving afterwards shows in the snapshot, because it is watched.
	a.State.SetDemo(true)
	a.State.FeedDemo([]byte(`{"context":"vessels.self","updates":[{"values":[{"path":"vendor.custom.thing","value":4.5}]}]}`), now)
	if r, ok := a.State.Snapshot().Own.Path("vendor.custom.thing"); !ok || r.V != 4.5 {
		t.Errorf("a path in a box should be in the snapshot: %+v %v", r, ok)
	}
	if f := saved(t, path); f.Boxes[1] != id {
		t.Errorf("a path box should be saved: %v", f.Boxes)
	}
	// Putting an ordinary kind back stops watching it.
	if err := a.SetBox(1, "sog"); err != nil {
		t.Fatal(err)
	}
	if _, ok := a.State.Snapshot().Own.Path("vendor.custom.thing"); ok {
		t.Error("a path that is no longer shown should not be watched")
	}
	// A path asked about is not asked about again straight away.
	a.mu.Lock()
	a.metaTried = nil
	a.mu.Unlock()
	a.State.SetMeta("vendor.custom.thing", "m/s")
	a.SetBox(1, id)
	select {
	case p := <-asked:
		t.Errorf("a path whose units are known was asked about again: %q", p)
	case <-time.After(100 * time.Millisecond):
	}
}

func TestSetBrightness(t *testing.T) {
	a, path := controlApp(t)
	if err := a.SetBrightness(5); err == nil {
		t.Error("with no front light there is nothing to set")
	}
	light := frontlight.NewFake(24, 3)
	a.Light = light
	a.InitLight()
	if err := a.SetBrightness(12); err != nil {
		t.Fatal(err)
	}
	if light.Current != 12 || a.Control().Light != 12 {
		t.Errorf("light = %d, control = %d, want 12", light.Current, a.Control().Light)
	}
	if f := saved(t, path); f.Brightness == nil || *f.Brightness != 12 {
		t.Errorf("the level should be saved: %+v", f.Brightness)
	}
	for _, bad := range []int{-1, 25, 1000} {
		if err := a.SetBrightness(bad); err == nil {
			t.Errorf("brightness %d should be refused", bad)
		}
	}
	if light.Current != 12 {
		t.Error("a refused level must leave the light alone")
	}
	if err := a.SetBrightness(0); err != nil || light.Current != 0 {
		t.Errorf("0 is off: %v, light %d", err, light.Current)
	}
}

func TestWindAndSpeedWidgetsAreNotSaved(t *testing.T) {
	a, path := controlApp(t)
	a.SetWindTrue(true)
	if !a.Control().WindTrue || !a.takeForce() {
		t.Error("the wind widget should switch, and redraw at once")
	}
	if err := a.SetSpeed("depth"); err != nil {
		t.Fatal(err)
	}
	if a.Control().Speed != "depth" || !a.takeForce() {
		t.Errorf("the speed widget should switch: %+v", a.Control())
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Error("the widgets start the same way on every boot, so nothing is saved for them")
	}
	if err := a.SetSpeed("sog"); err != nil || a.Control().Speed != "sog" {
		t.Errorf("back to the default: %v %+v", err, a.Control())
	}
	for _, bad := range []string{"nope", pages.BoxAIS, "path:a b"} {
		if err := a.SetSpeed(bad); err == nil {
			t.Errorf("the speed widget cannot show %q", bad)
		}
	}
	if err := a.SetSpeed(pages.PathKindID("environment.outside.pressure")); err != nil {
		t.Errorf("a path should do: %v", err)
	}
	// A tap on the widget, set to something else, goes back to SOG, and the path
	// stops being watched.
	if got := a.speed.Next(); got != pages.SpeedSOG {
		t.Errorf("tap from a path should reach SOG, got %v", got)
	}
}

func TestTheWidgetShowsWhatItWasSetTo(t *testing.T) {
	a, _ := controlApp(t)
	a.State.SetDemo(true)
	now := time.Now()
	a.State.FeedDemo([]byte(`{"context":"vessels.self","updates":[{"values":[{"path":"environment.depth.belowTransducer","value":12.3},{"path":"navigation.headingTrue","value":1.0},{"path":"navigation.speedOverGround","value":3}]}]}`), now)
	frame := func() *image.Gray {
		img, err := a.Frame(now)
		if err != nil {
			t.Fatal(err)
		}
		return img
	}
	box := pages.SpeedBoxRect(image.Rect(0, 0, 1072, 1448))
	differs := func(x, y *image.Gray) bool {
		for py := box.Min.Y; py < box.Max.Y; py++ {
			for px := box.Min.X; px < box.Max.X; px++ {
				if x.GrayAt(px, py).Y != y.GrayAt(px, py).Y {
					return true
				}
			}
		}
		return false
	}
	sog := frame()
	if err := a.SetSpeed("depth"); err != nil {
		t.Fatal(err)
	}
	depth := frame()
	if !differs(sog, depth) {
		t.Error("the speed box looks the same after being set to depth")
	}
	if err := a.SetSpeed("sog"); err != nil {
		t.Fatal(err)
	}
	if differs(sog, frame()) {
		t.Error("setting it back to SOG should draw exactly what it did before")
	}
}
