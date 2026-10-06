package app

import (
	"errors"
	"image"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"

	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// newMoreApp is an app with no front light, so the root list is one row shorter.
func newMoreApp(t *testing.T) (*App, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "settings.json")
	a := &App{State: signalk.NewState(), Display: &display.PNG{W: 1072, H: 1448, Path: filepath.Join(t.TempDir(), "f.png")}, SettingsPath: path}
	a.SetPage("nav")
	a.takePageChanged()
	return a, path
}

// openMore taps through to the more screen.
func openMore(a *App) {
	a.HandleEvent(tap(30, 40)) // the cog
	a.HandleEvent(tap(500, pages.SettingsRowY(len(units.Metrics)+5)))
}

func TestMoreScreenIsReachableFromTheCog(t *testing.T) {
	a, _ := newMoreApp(t)
	openMore(a)
	if a.settingsView.Screen != pages.SettingsMore {
		t.Fatalf("view = %+v, want the more screen", a.settingsView)
	}
	// Back goes to the list, and back again closes settings.
	a.HandleEvent(tap(30, 40))
	if !a.settingsOpen || a.settingsView.Screen != pages.SettingsRoot {
		t.Errorf("back from more: %+v (open %v)", a.settingsView, a.settingsOpen)
	}
	a.HandleEvent(tap(30, 40))
	if a.settingsOpen {
		t.Error("back from the list closes settings")
	}
}

func TestIdleSwitchFromTheScreen(t *testing.T) {
	a, path := newMoreApp(t)
	openMore(a)
	a.HandleEvent(tap(500, pages.SettingsRowY(1)))
	if !a.IdleEnabled {
		t.Fatal("the idle row should turn idle mode on")
	}
	if a.settingsView.Screen != pages.SettingsMore {
		t.Errorf("it should stay on the screen that says On, got %+v", a.settingsView)
	}
	if f, _ := settings.Load(path); !f.IdleEnabled {
		t.Error("not saved")
	}
	// The state is told which switch to keep: a value for it is now known.
	a.State.ApplyForTest(settings.DefaultIdlePath, 0)
	if _, known, _ := a.State.Switch(); !known {
		t.Error("the state is not watching the default switch")
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(1)))
	if a.IdleEnabled {
		t.Error("a second tap turns it off")
	}
	if _, known, _ := a.State.Switch(); known {
		t.Error("off means no switch is watched")
	}
}

func TestTimeZoneFromTheScreen(t *testing.T) {
	a, path := newMoreApp(t)
	openMore(a)
	a.HandleEvent(tap(500, pages.SettingsRowY(0)))
	if a.settingsView.Screen != pages.SettingsZone {
		t.Fatalf("view = %+v, want the zone picker", a.settingsView)
	}
	utc := 1 // ZoneChoices[1] is UTC
	if pages.ZoneChoices[utc].Name != "UTC" {
		t.Fatal("the test assumes UTC is the second choice")
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(utc)))
	if a.Timezone != "UTC" || a.settingsView.Screen != pages.SettingsMore {
		t.Errorf("zone %q, view %+v", a.Timezone, a.settingsView)
	}
	if f, _ := settings.Load(path); f.Timezone != "UTC" {
		t.Errorf("saved %+v", f)
	}

	// Another page: Sydney is further down the list.
	a.HandleEvent(tap(500, pages.SettingsRowY(0)))
	want := "Australia/Sydney"
	idx := -1
	for i, z := range pages.ZoneChoices {
		if z.Name == want {
			idx = i
		}
	}
	page := idx / pages.ZonesPerPage
	for p := a.settingsView.Page; p < page; p++ {
		a.HandleEvent(tap(850, 1170)) // NEXT
	}
	if a.settingsView.Page != page {
		t.Fatalf("on page %d, want %d", a.settingsView.Page, page)
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(idx-page*pages.ZonesPerPage)))
	if a.Timezone != want {
		t.Errorf("zone = %q, want %q", a.Timezone, want)
	}
	// Opening the picker again lands on the page of the zone in force.
	a.HandleEvent(tap(500, pages.SettingsRowY(0)))
	if a.settingsView.Page != page {
		t.Errorf("the picker opened on page %d, want %d (where %s is)", a.settingsView.Page, page, want)
	}
	// And the device's own zone is the first choice on the first page.
	a.settingsView.Page = 0
	a.HandleEvent(tap(500, pages.SettingsRowY(0)))
	if a.Timezone != "" {
		t.Errorf("the device default should clear the zone, got %q", a.Timezone)
	}
	if f, _ := settings.Load(path); f.Timezone != "" {
		t.Errorf("saved %+v", f)
	}
}

func TestEveryClockReadsInTheChosenZone(t *testing.T) {
	a, path := newMoreApp(t)
	at := time.Date(2026, 1, 15, 0, 30, 0, 0, time.UTC) // 13:30 in Auckland, 11:30 in Brisbane

	if err := a.SetTimezone("Pacific/Auckland"); err != nil {
		t.Fatal(err)
	}
	a.mu.Lock()
	got := a.clockLocked(at)
	a.mu.Unlock()
	if got != "13:30" {
		t.Errorf("Auckland clock = %s, want 13:30", got)
	}
	auckland, err := a.Frame(at)
	if err != nil {
		t.Fatal(err)
	}
	if err := a.SetTimezone("Australia/Brisbane"); err != nil {
		t.Fatal(err)
	}
	brisbane, _ := a.Frame(at)
	if c := a.Control(); c.Timezone != "Australia/Brisbane" || c.Clock == "" {
		t.Errorf("Control = %+v", c)
	}
	// The header clock is at the top right: the two zones' frames differ there.
	same := true
	for y := 20; y < 80 && same; y++ {
		for x := 700; x < 1040; x++ {
			if auckland.GrayAt(x, y) != brisbane.GrayAt(x, y) {
				same = false
				break
			}
		}
	}
	if same {
		t.Error("the header clock did not change with the zone")
	}
	// The clock the page draws is the zone's: drawing it again in a third zone that
	// shares Brisbane's minutes but not its hour keeps differing.
	if f, _ := settings.Load(path); f.Timezone != "Australia/Brisbane" {
		t.Errorf("saved %+v", f)
	}

	// A bad name is refused and changes nothing; "Local" is the device's own.
	if err := a.SetTimezone("Mars/Olympus"); err == nil {
		t.Error("a bad zone was accepted")
	}
	if a.Control().Timezone != "Australia/Brisbane" {
		t.Error("a refused zone changed the zone")
	}
	if err := a.SetTimezone("Local"); err != nil || a.Control().Timezone != "" {
		t.Errorf("Local: %v, zone %q", err, a.Control().Timezone)
	}
}

func TestApplyTimezoneAtStartUp(t *testing.T) {
	a, path := newMoreApp(t)
	a.ApplyTimezone("Australia/Sydney")
	if a.Control().Timezone != "Australia/Sydney" {
		t.Errorf("zone = %q", a.Control().Timezone)
	}
	if _, err := settings.Load(path); err != nil {
		t.Fatal(err)
	}
	if f, _ := settings.Load(path); f.Timezone != "" {
		t.Error("applying a saved zone at start-up must not write the file")
	}
	// An unusable saved zone falls back to the device's own, rather than refusing to start.
	a.ApplyTimezone("Mars/Olympus")
	if a.Control().Timezone != "Australia/Sydney" {
		t.Errorf("a bad name should change nothing, got %q", a.Control().Timezone)
	}
}

// --- software update ---------------------------------------------------------

func fastRestart(t *testing.T) {
	t.Helper()
	old := updateRestartDelay
	updateRestartDelay = 5 * time.Millisecond
	t.Cleanup(func() { updateRestartDelay = old })
}

func TestUpdateNowReportsWhatItFound(t *testing.T) {
	fastRestart(t)
	for name, tc := range map[string]struct {
		res       UpdateResult
		err       error
		wantMsg   string
		wantRestr bool
	}{
		"up to date": {UpdateResult{Current: 49, Newest: 49}, nil, "Up to date: v49 is the newest", false},
		"installed":  {UpdateResult{Current: 49, Newest: 50, Installed: true}, nil, "Installed v50: restarting...", true},
		"failed":     {UpdateResult{}, errors.New("no route to host"), "Update failed: no route to host", false},
	} {
		a, _ := newMoreApp(t)
		var restarts atomic.Int32
		a.OnUpdate = func() (UpdateResult, error) { return tc.res, tc.err }
		a.OnRestart = func() { restarts.Add(1) }
		if !a.Control().CanUpdate {
			t.Fatalf("%s: with an updater it can update", name)
		}
		if err := a.UpdateNow(); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		waitUntil(t, name+": the answer", func() bool { c := a.Control(); return !c.UpdateBusy && c.UpdateMsg == tc.wantMsg })
		if tc.wantRestr {
			waitUntil(t, name+": the restart", func() bool { return restarts.Load() == 1 })
		} else {
			time.Sleep(60 * time.Millisecond)
			if restarts.Load() != 0 {
				t.Errorf("%s: restarted without installing anything", name)
			}
		}
		if restarts.Load() > 1 {
			t.Errorf("%s: restarted %d times", name, restarts.Load())
		}
	}
}

func TestUpdateNowWithNoUpdaterSaysSo(t *testing.T) {
	a, _ := newMoreApp(t)
	if a.Control().CanUpdate {
		t.Error("a PC preview cannot update itself")
	}
	if err := a.UpdateNow(); err == nil {
		t.Error("expected an error")
	}
	if a.Control().UpdateBusy {
		t.Error("nothing was started")
	}
}

func TestUpdateNowIsNotStartedTwice(t *testing.T) {
	fastRestart(t)
	a, _ := newMoreApp(t)
	var calls atomic.Int32
	release := make(chan struct{})
	a.OnUpdate = func() (UpdateResult, error) {
		calls.Add(1)
		<-release
		return UpdateResult{Current: 49, Newest: 49}, nil
	}
	if err := a.UpdateNow(); err != nil {
		t.Fatal(err)
	}
	waitUntil(t, "the check to start", func() bool { return calls.Load() == 1 })
	if !a.Control().UpdateBusy || a.Control().UpdateMsg == "" {
		t.Errorf("while checking: %+v", a.Control())
	}
	a.UpdateNow()
	a.UpdateNow()
	close(release)
	waitUntil(t, "it to finish", func() bool { return !a.Control().UpdateBusy })
	if calls.Load() != 1 {
		t.Errorf("the updater ran %d times, want once", calls.Load())
	}
}

func TestUpdateFromTheScreenShowsProgressOnTheMoreScreen(t *testing.T) {
	fastRestart(t)
	a, _ := newMoreApp(t)
	release := make(chan struct{})
	a.OnUpdate = func() (UpdateResult, error) { <-release; return UpdateResult{Current: 49, Newest: 49}, nil }
	openMore(a)
	before, _ := a.Frame(time.Now())
	updRow := image.Rect(500, pages.SettingsRowY(3)-36, 1072, pages.SettingsRowY(3)+36)
	snap := func(img *image.Gray) []byte {
		var b []byte
		for y := updRow.Min.Y; y < updRow.Max.Y; y++ {
			for x := updRow.Min.X; x < updRow.Max.X; x++ {
				b = append(b, img.GrayAt(x, y).Y)
			}
		}
		return b
	}

	a.HandleEvent(tap(500, pages.SettingsRowY(3)))
	waitUntil(t, "the check to start", func() bool { return a.Control().UpdateBusy })
	working, _ := a.Frame(time.Now())
	if string(snap(before)) == string(snap(working)) {
		t.Error("the row should say it is working")
	}
	// Tapping again while it works starts nothing more.
	a.HandleEvent(tap(500, pages.SettingsRowY(3)))
	close(release)
	waitUntil(t, "the answer", func() bool { return a.Control().UpdateMsg == "Up to date: v49 is the newest" })
	done, _ := a.Frame(time.Now())
	if string(snap(done)) != string(snap(before)) {
		t.Error("when it has finished the row goes back to showing the version")
	}
}
