package app

import (
	"image"
	"os"
	"path/filepath"
	"testing"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/signalk"
)

func mapApp(t *testing.T, w, h int) *App {
	t.Helper()
	a := &App{
		State:        signalk.NewState(),
		Display:      &display.PNG{W: w, H: h, Path: filepath.Join(t.TempDir(), "f.png")},
		SettingsPath: filepath.Join(t.TempDir(), "settings.json"),
	}
	if !a.SetPage("map") {
		t.Fatal("no map page")
	}
	a.takePageChanged()
	return a
}

// at is the centre of a design-space rectangle, as a touch at device pixels on a
// screen w wide.
func at(r image.Rectangle, w int) (int, int) {
	s := float64(w) / float64(pages.DesignWidth)
	return int(float64(r.Min.X+r.Max.X) / 2 * s), int(float64(r.Min.Y+r.Max.Y) / 2 * s)
}

func TestMapTapsChangeRangeAndOrientation(t *testing.T) {
	a := mapApp(t, 1072, 1448)
	b := image.Rect(0, 0, 1072, 1448)

	if got := a.Control(); got.MapRange != 5 || got.MapNorthUp || got.Page != "map" {
		t.Fatalf("start: %+v", got)
	}
	x, y := at(pages.MapRangeRect(b), 1072)
	for i, want := range []int{10, 1, 2, 5} {
		a.HandleEvent(tap(x, y))
		if got := a.Control().MapRange; got != want {
			t.Fatalf("tap %d: range %d, want %d", i+1, got, want)
		}
		if !a.takeForce() {
			t.Errorf("tap %d: a change in place should redraw at once", i+1)
		}
	}
	if a.Control().Page != "map" {
		t.Error("a tap on the range paged away")
	}

	x, y = at(pages.MapOrientRect(b), 1072)
	a.HandleEvent(tap(x, y))
	if !a.Control().MapNorthUp {
		t.Error("a tap on the orientation should put north up")
	}
	a.HandleEvent(tap(x, y))
	if a.Control().MapNorthUp {
		t.Error("and again, heading up")
	}
	if a.Control().Page != "map" {
		t.Error("a tap on the orientation paged away (it is in the left third)")
	}
	// Neither is saved: they start the same way on every boot.
	if _, err := os.Stat(a.SettingsPath); !os.IsNotExist(err) {
		t.Error("the map's range and orientation should not be saved")
	}
}

func TestMapWindWidgetTapNeedsWind(t *testing.T) {
	a := mapApp(t, 1072, 1448)
	b := image.Rect(0, 0, 1072, 1448)
	x, y := at(pages.MapWindRect(b), 1072)

	// With no wind speed the widget is not there: the tap pages as it always did
	// (the left third goes to the previous page).
	a.HandleEvent(tap(x, y))
	if a.Control().WindTrue {
		t.Error("no wind, nothing to switch")
	}
	if a.Control().Page != "nav" {
		t.Errorf("with no wind widget a left-third tap should page back, got %s", a.Control().Page)
	}

	// With wind, it switches true and apparent, as on the compass.
	a = mapApp(t, 1072, 1448)
	a.State.SetDemo(true)
	a.State.FeedDemo([]byte(`{"context":"vessels.self","updates":[{"values":[{"path":"environment.wind.speedApparent","value":8}]}]}`), time.Now())
	a.HandleEvent(tap(x, y))
	if !a.Control().WindTrue || a.Control().Page != "map" {
		t.Errorf("wind widget tap: WindTrue=%v page=%s", a.Control().WindTrue, a.Control().Page)
	}
	a.HandleEvent(tap(x, y))
	if a.Control().WindTrue {
		t.Error("and back to apparent")
	}
}

func TestMapTapsOnASmallScreen(t *testing.T) {
	a := mapApp(t, 600, 800)
	b := image.Rect(0, 0, 1072, 1448)
	x, y := at(pages.MapRangeRect(b), 600)
	a.HandleEvent(tap(x, y))
	if a.Control().MapRange != 10 {
		t.Errorf("a tap on the range, on a 600-wide screen, left it at %d", a.Control().MapRange)
	}
	x, y = at(pages.MapOrientRect(b), 600)
	a.HandleEvent(tap(x, y))
	if !a.Control().MapNorthUp {
		t.Error("a tap on the orientation on a small screen did nothing")
	}
}

func TestTapsElsewhereOnTheMapStillPage(t *testing.T) {
	a := mapApp(t, 1072, 1448)
	a.HandleEvent(tap(1000, 700)) // right third, away from the corners
	if a.Control().Page != "compass" {
		t.Errorf("a right-third tap on the map should go to the next page (compass), got %s", a.Control().Page)
	}
	a.SetPage("map")
	a.HandleEvent(tap(540, 700)) // the middle: the map itself is not a button
	if a.Control().Page != "map" || a.Control().MapRange != 5 || a.Control().MapNorthUp {
		t.Errorf("a tap on the middle of the map did something: %+v", a.Control())
	}
}

func TestMapRemoteControls(t *testing.T) {
	a := mapApp(t, 1072, 1448)
	if err := a.SetMapRange(2); err != nil || a.Control().MapRange != 2 {
		t.Errorf("SetMapRange(2): %v, now %d", err, a.Control().MapRange)
	}
	if !a.takeForce() {
		t.Error("a new range should redraw at once")
	}
	if err := a.SetMapRange(5); err != nil || a.Control().MapRange != 5 {
		t.Errorf("SetMapRange(5): %v, now %d", err, a.Control().MapRange)
	}
	// 5 is the default, stored as such: a tap from it goes to 10.
	x, y := at(pages.MapRangeRect(image.Rect(0, 0, 1072, 1448)), 1072)
	a.HandleEvent(tap(x, y))
	if a.Control().MapRange != 10 {
		t.Errorf("a tap after setting 5 should go to 10, got %d", a.Control().MapRange)
	}
	for _, bad := range []int{0, 3, -1, 7, 100} {
		if err := a.SetMapRange(bad); err == nil {
			t.Errorf("%d nm should be refused", bad)
		}
	}
	if a.Control().MapRange != 10 {
		t.Error("a refused range must change nothing")
	}
	a.SetMapNorthUp(true)
	if !a.Control().MapNorthUp || !a.takeForce() {
		t.Error("north up should be set, and redrawn at once")
	}
	a.SetMapNorthUp(false)
	if a.Control().MapNorthUp {
		t.Error("and heading up again")
	}
	if got := a.Control().MapRanges; len(got) != 4 || got[2] != 5 {
		t.Errorf("the ranges offered: %v", got)
	}
	if _, err := os.Stat(a.SettingsPath); !os.IsNotExist(err) {
		t.Error("nothing about the map is saved")
	}
}

func TestMapPageIsListedAndDrawn(t *testing.T) {
	a := mapApp(t, 1072, 1448)
	found := false
	for _, p := range a.Control().Pages {
		found = found || (p.ID == "map" && p.Name == "Map")
	}
	if !found {
		t.Errorf("pages: %+v", a.Control().Pages)
	}
	img, err := a.Frame(time.Now())
	if err != nil {
		t.Fatal(err)
	}
	dark := 0
	for y := 300; y < 1000; y += 3 {
		for x := 100; x < 1000; x += 3 {
			if img.GrayAt(x, y).Y < 100 {
				dark++
			}
		}
	}
	if dark == 0 {
		t.Error("the map page drew nothing")
	}
}
