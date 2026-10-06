package app

import (
	"testing"
	"time"

	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/units"
)

const (
	ownPos = `{"context":"vessels.self","updates":[{"values":[{"path":"navigation.position","value":{"latitude":-33.8,"longitude":151.2}}]}]}`
	aisMsg = `{"context":"vessels.urn:mrn:imo:mmsi:235000001","updates":[{"values":[{"path":"navigation.speedOverGround","value":3}]}]}`
)

// headerIsBlack reports whether the header strip shows the black NO DATA banner.
func headerIsBlack(t *testing.T, a *App, at time.Time) bool {
	t.Helper()
	img, err := a.Frame(at)
	if err != nil {
		t.Fatal(err)
	}
	dark := 0
	for y := 8; y < 30; y++ {
		for x := 300; x < 500; x++ {
			if img.GrayAt(x, y).Y < 60 {
				dark++
			}
		}
	}
	return dark > 200*22*9/10
}

func TestTheBannerFollowsOurOwnGPSNotOtherVesselsReports(t *testing.T) {
	a, _ := newMoreApp(t)
	a.State.SetDemo(true) // a stand-in server, connected, that FeedDemo can speak for
	a.SetPage("nav")
	t0 := time.Now()
	a.State.FeedDemo([]byte(ownPos), t0)

	a.NoGPSSec = 10
	if headerIsBlack(t, a, t0.Add(2*time.Second)) {
		t.Error("the GPS fix is two seconds old: no banner")
	}

	// Our GPS goes quiet, but a ship's AIS report keeps the server from being silent.
	a.State.FeedDemo([]byte(aisMsg), t0.Add(11*time.Second))
	if !headerIsBlack(t, a, t0.Add(12*time.Second)) {
		t.Error("our GPS has said nothing for 12 s although AIS is arriving: the banner should be up")
	}
	// With the check off that is not noticed (the old behaviour).
	a.NoGPSSec = 0
	if headerIsBlack(t, a, t0.Add(12*time.Second)) {
		t.Error("with the GPS check off an AIS report keeps the banner away")
	}
	// A longer limit has not been reached yet.
	a.NoGPSSec = 30
	if headerIsBlack(t, a, t0.Add(12*time.Second)) {
		t.Error("30 s were allowed and only 12 have passed")
	}

	// The fix comes back: the banner goes.
	a.NoGPSSec = 10
	a.State.FeedDemo([]byte(ownPos), t0.Add(13*time.Second))
	if headerIsBlack(t, a, t0.Add(14*time.Second)) {
		t.Error("the GPS is back: no banner")
	}
}

func TestSettingTheGPSTimeout(t *testing.T) {
	a, path := newMoreApp(t)
	if err := a.SetNoGPSSeconds(30); err != nil {
		t.Fatal(err)
	}
	if a.Control().NoGPSSec != 30 {
		t.Errorf("Control = %d", a.Control().NoGPSSec)
	}
	if f, _ := settings.Load(path); f.NoGPSSeconds == nil || *f.NoGPSSeconds != 30 {
		t.Errorf("not saved: %v", f.NoGPSSeconds)
	}
	for _, bad := range []int{1, -3, 3601} {
		if err := a.SetNoGPSSeconds(bad); err == nil {
			t.Errorf("%d was accepted", bad)
		}
	}
	if a.Control().NoGPSSec != 30 {
		t.Error("a refused value changed the setting")
	}
	// Off is a choice, and is saved as one.
	if err := a.SetNoGPSSeconds(0); err != nil {
		t.Fatal(err)
	}
	if f, _ := settings.Load(path); f.NoGPSSeconds == nil || f.NoGPS() != 0 {
		t.Errorf("off was not saved as off: %v", f.NoGPSSeconds)
	}
}

func TestGPSTimeoutFromTheScreen(t *testing.T) {
	a, path := newMoreApp(t)
	openMore(a)
	a.HandleEvent(tap(500, pages.SettingsRowY(2)))
	if a.settingsView.Screen != pages.SettingsNoGPS {
		t.Fatalf("view = %+v, want the GPS picker", a.settingsView)
	}
	thirty := -1
	for i, ch := range pages.NoGPSChoices {
		if ch.Seconds == 30 {
			thirty = i
		}
	}
	a.HandleEvent(tap(500, pages.SettingsRowY(thirty)))
	if a.NoGPSSec != 30 || a.settingsView.Screen != pages.SettingsMore {
		t.Errorf("seconds %d, view %+v", a.NoGPSSec, a.settingsView)
	}
	if f, _ := settings.Load(path); f.NoGPSSeconds == nil || *f.NoGPSSeconds != 30 {
		t.Errorf("saved %v", f.NoGPSSeconds)
	}

	// Off, from the same screen.
	a.HandleEvent(tap(500, pages.SettingsRowY(2)))
	a.HandleEvent(tap(500, pages.SettingsRowY(len(pages.NoGPSChoices)-1)))
	if a.NoGPSSec != 0 {
		t.Errorf("Off left %d", a.NoGPSSec)
	}
	if f, _ := settings.Load(path); f.NoGPSSeconds == nil || f.NoGPS() != 0 {
		t.Errorf("off was not saved: %v", f.NoGPSSeconds)
	}
	// The other rows of the root list are where they were: the new row is on the more screen.
	_ = units.Metrics
}
