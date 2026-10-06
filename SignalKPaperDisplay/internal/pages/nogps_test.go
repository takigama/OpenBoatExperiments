package pages

import (
	"image"
	"testing"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
)

// snap is a connected server that has just said something, with our GPS position last
// updated posAge ago (zero: never).
func snap(now time.Time, lastMsgAge, posAge time.Duration) signalk.Snapshot {
	s := signalk.Snapshot{Connected: true, LastMessage: now.Add(-lastMsgAge)}
	if posAge != 0 {
		s.Own.Pos = signalk.Position{Lat: -33.8, Lon: 151.2, At: now.Add(-posAge)}
	}
	return s
}

func TestLostByGPSSilence(t *testing.T) {
	now := time.Date(2026, 10, 6, 12, 0, 0, 0, time.UTC)
	const gps = 10 * time.Second
	for _, tc := range []struct {
		name string
		s    signalk.Snapshot
		gps  time.Duration
		want bool
	}{
		{"everything is arriving", snap(now, time.Second, 2*time.Second), gps, false},
		{"the position is just inside the limit", snap(now, time.Second, 10*time.Second), gps, false},
		{"a moment past it", snap(now, time.Second, 10*time.Second+100*time.Millisecond), gps, true},
		// The case this exists for: other vessels' AIS reports keep arriving, so the
		// server is not silent, but our own position has stopped.
		{"AIS still arriving, own GPS gone quiet", snap(now, time.Second, time.Minute), gps, true},
		{"a position never received", snap(now, time.Second, 0), gps, true},
		{"the check is off: a silent GPS is not noticed", snap(now, time.Second, time.Hour), 0, false},
		{"off, and never a position", snap(now, time.Second, 0), 0, false},
		// The older rules are unchanged, whatever the GPS says.
		{"nothing at all for 6 s", snap(now, 6*time.Second, 2*time.Second), gps, true},
		{"nothing at all for 6 s, check off", snap(now, 6*time.Second, 2*time.Second), 0, true},
		{"link down", signalk.Snapshot{Connected: false, LastMessage: now, Own: signalk.Own{Pos: signalk.Position{At: now}}}, gps, true},
		{"a longer limit tolerates a longer gap", snap(now, time.Second, 25*time.Second), 30 * time.Second, false},
	} {
		if got := Lost(tc.s, now, tc.gps); got != tc.want {
			t.Errorf("%s: Lost = %v, want %v", tc.name, got, tc.want)
		}
	}
}

func TestTheHeaderSaysNoDataWhenTheGPSIsSilent(t *testing.T) {
	now := time.Date(2026, 10, 6, 12, 0, 0, 0, time.UTC)
	draw := func(s signalk.Snapshot, gps time.Duration) *render.Canvas {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Header(c, "NAV", s, now, Env{GPSTimeout: gps})
		return c
	}
	// The banner fills the header with black; a normal header is white between the
	// title and the clock.
	probe := image.Rect(300, 8, 500, 30)
	if inked(draw(snap(now, time.Second, time.Second), 10*time.Second), probe) != 0 {
		t.Fatal("setup: a healthy header should be white here")
	}
	if got := inked(draw(snap(now, time.Second, time.Minute), 10*time.Second), probe); got < probe.Dx()*probe.Dy()*9/10 {
		t.Errorf("a silent GPS should black out the header, only %d of %d pixels are inked", got, probe.Dx()*probe.Dy())
	}
	if inked(draw(snap(now, time.Second, time.Minute), 0), probe) != 0 {
		t.Error("with the check off a silent GPS must not show the banner")
	}
}

func TestMoreScreenOpensTheGPSPicker(t *testing.T) {
	v := SettingsView{Screen: SettingsMore}
	if got := tapAt(v, SettingsRowY(moreGPSRow)); got.Kind != ActOpenNoGPS {
		t.Errorf("the NO DATA row = %+v", got)
	}
	if ParentScreen(SettingsNoGPS) != SettingsMore {
		t.Error("back from the picker goes to the more screen")
	}
	if moreGPSRow == moreIdleRow || moreGPSRow == moreZoneRow || moreGPSRow == moreUpdateRow {
		t.Error("the rows overlap")
	}
}

func TestGPSPickerChoices(t *testing.T) {
	v := SettingsView{Screen: SettingsNoGPS}
	for i, ch := range NoGPSChoices {
		got := tapAt(v, SettingsRowY(i))
		if got.Kind != ActSetNoGPS || got.Level != ch.Seconds {
			t.Errorf("row %d (%s) = %+v", i, ch.Label, got)
		}
		if !settings.ValidNoGPS(ch.Seconds) {
			t.Errorf("%s offers %d seconds, which cannot be set", ch.Label, ch.Seconds)
		}
	}
	if got := tapAt(v, SettingsRowY(len(NoGPSChoices))); got.Kind != ActNone {
		t.Errorf("below the choices = %+v", got)
	}
	if NoGPSChoices[len(NoGPSChoices)-1].Seconds != 0 {
		t.Error("Off is the last choice")
	}
	var def bool
	for _, ch := range NoGPSChoices {
		def = def || ch.Seconds == settings.DefaultNoGPSSeconds
	}
	if !def {
		t.Errorf("the default (%d s) is not among the choices", settings.DefaultNoGPSSeconds)
	}

	// The chosen one is marked.
	on := drawSettings(t, SettingsView{Screen: SettingsNoGPS, NoGPS: 30})
	off := drawSettings(t, SettingsView{Screen: SettingsNoGPS, NoGPS: 0})
	row := 0
	for i, ch := range NoGPSChoices {
		if ch.Seconds == 30 {
			row = i
		}
	}
	r := image.Rect(960, SettingsRowY(row)-36, 1072, SettingsRowY(row)+36)
	if inked(on, r) <= inked(off, r) {
		t.Error("the chosen timeout's radio button is not filled")
	}
	// The notes under the choices stay on the screen.
	if bottom := settingsTop + len(NoGPSChoices)*settingsRowH + 60 + 2*46; bottom > 1300 {
		t.Errorf("the notes run too low (%d)", bottom)
	}
}

func TestNoGPSLabels(t *testing.T) {
	for in, want := range map[int]string{0: "Off", 5: "5 seconds", 10: "10 seconds", 60: "1 minute", 300: "5 minutes", 45: "45 seconds", 120: "2 minutes", 90: "90 seconds"} {
		if got := NoGPSLabel(in); got != want {
			t.Errorf("NoGPSLabel(%d) = %q, want %q", in, got, want)
		}
	}
}

func TestMoreScreenShowsTheGPSTimeout(t *testing.T) {
	base := SettingsView{Screen: SettingsMore, IdlePath: settings.DefaultIdlePath, NoGPS: 10}
	a := drawSettings(t, base)
	other := base
	other.NoGPS = 30
	row := image.Rect(500, SettingsRowY(moreGPSRow)-36, 960, SettingsRowY(moreGPSRow)+36)
	if !differsIn(a, drawSettings(t, other), row) {
		t.Error("the row does not show the timeout")
	}
	// The sentence under it changes with it too.
	if !differsIn(a, drawSettings(t, other), image.Rect(40, 792, 1040, 835)) {
		t.Error("the note does not show the timeout")
	}
}

// The sentence under the row must stay on the screen whatever the setting.
func TestGPSNoteFitsForEverySetting(t *testing.T) {
	for _, sec := range []int{0, 2, 5, 10, 30, 45, 60, 90, 300, 3600} {
		c := drawSettings(t, SettingsView{Screen: SettingsMore, IdlePath: settings.DefaultIdlePath, NoGPS: sec})
		if n := inked(c, image.Rect(1056, 740, 1072, 840)); n != 0 {
			t.Errorf("%d s: the note runs to the right edge of the screen (%d inked pixels)", sec, n)
		}
		if inked(c, image.Rect(40, 792, 1040, 835)) == 0 {
			t.Errorf("%d s: the second line is not where the test looks", sec)
		}
	}
}
