package pages

import (
	"image"
	"strings"
	"testing"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/settings"
)

const wMore = 1072

func tapAt(v SettingsView, y int) Action { return SettingsTap(v, 500, y, wMore) }

func TestRootOpensTheMoreScreenAndBackComesBack(t *testing.T) {
	for _, hasLight := range []bool{false, true} {
		v := SettingsView{}
		if hasLight {
			v.MaxLevel = 24
		}
		if got := tapAt(v, SettingsRowY(moreRow(hasLight))); got.Kind != ActOpenMore {
			t.Errorf("light=%v: the more row = %+v", hasLight, got)
		}
		if moreRow(hasLight) != demoRow(hasLight)+1 || powerRow(hasLight) != moreRow(hasLight)+1 {
			t.Errorf("light=%v: rows are demo %d, more %d, power %d", hasLight, demoRow(hasLight), moreRow(hasLight), powerRow(hasLight))
		}
	}
	if ParentScreen(SettingsMore) != SettingsRoot {
		t.Error("back from the more screen goes to the list")
	}
	if ParentScreen(SettingsZone) != SettingsMore {
		t.Error("back from the zone picker goes to the more screen")
	}
}

func TestMoreScreenRows(t *testing.T) {
	v := SettingsView{Screen: SettingsMore}
	if got := tapAt(v, SettingsRowY(moreZoneRow)); got.Kind != ActOpenZone {
		t.Errorf("time zone row = %+v", got)
	}
	if got := tapAt(v, SettingsRowY(moreIdleRow)); got.Kind != ActToggleIdle {
		t.Errorf("idle row = %+v", got)
	}
	if got := tapAt(v, SettingsRowY(moreUpdateRow)); got.Kind != ActUpdateNow {
		t.Errorf("update row = %+v", got)
	}
	// A check that is already running is not started again.
	busy := v
	busy.UpdateBusy = true
	if got := tapAt(busy, SettingsRowY(moreUpdateRow)); got.Kind != ActNone {
		t.Errorf("update row while checking = %+v, want nothing", got)
	}
	if got := tapAt(v, SettingsRowY(moreUpdateRow+1)); got.Kind != ActNone {
		t.Errorf("below the rows = %+v, want nothing", got)
	}
	// The cog corner is Back, as on every settings screen.
	if got := SettingsTap(v, 30, 40, wMore); got.Kind != ActBack {
		t.Errorf("the corner = %+v", got)
	}
}

func TestMoreScreenShowsItsState(t *testing.T) {
	base := SettingsView{Screen: SettingsMore, IdlePath: settings.DefaultIdlePath}
	plain := drawSettings(t, base)
	if inked(plain, image.Rect(0, 0, 1072, 1448)) == 0 {
		t.Fatal("nothing drawn")
	}
	idleOn := base
	idleOn.IdleOn = true
	zone := base
	zone.Zone = "Australia/Sydney"
	busy := base
	busy.UpdateBusy = true
	msg := base
	msg.UpdateMsg = "Up to date: v50 is the newest"
	clock := base
	clock.Clock = "14:32"

	idleRow := image.Rect(700, SettingsRowY(moreIdleRow)-36, 1072, SettingsRowY(moreIdleRow)+36)
	zoneRow := image.Rect(500, SettingsRowY(moreZoneRow)-36, 960, SettingsRowY(moreZoneRow)+36)
	updRow := image.Rect(500, SettingsRowY(moreUpdateRow)-36, 1072, SettingsRowY(moreUpdateRow)+36)
	for name, tc := range map[string]struct {
		v SettingsView
		r image.Rectangle
	}{
		"idle On vs Off":     {idleOn, idleRow},
		"the zone's name":    {zone, zoneRow},
		"the update working": {busy, updRow},
		"the update message": {msg, image.Rect(40, 740, 1040, 830)},
		"the clock":          {clock, image.Rect(40, 370, 1040, 430)},
	} {
		if !differsIn(plain, drawSettings(t, tc.v), tc.r) {
			t.Errorf("%s makes no difference to the screen", name)
		}
	}
}

// What changes while the screen is up - the clock, the update's progress, the
// switch's path - is redrawn in place under the fast waveform, which has no grey.
func TestMoreScreenLiveTextIsSolidBlack(t *testing.T) {
	v := SettingsView{Screen: SettingsMore, Clock: "14:32", IdlePath: settings.DefaultIdlePath, UpdateMsg: "Installed v50: restarting..."}
	c := drawSettings(t, v)
	for name, r := range map[string]image.Rectangle{
		"the clock":   image.Rect(40, 380, 1040, 425),
		"the path":    image.Rect(40, 565, 1040, 615),
		"the message": image.Rect(40, 760, 1040, 815),
	} {
		if got := darkest(c, r); got > 10 {
			t.Errorf("%s is drawn in grey %d; it must be solid black", name, got)
		}
		if inked(c, r) == 0 {
			t.Errorf("%s is not where the test looks (nothing drawn in %v)", name, r)
		}
	}
}

func TestMoreScreenFitsAboveTheVersionLabel(t *testing.T) {
	// The update message is the lowest thing on the screen.
	if bottom := settingsTop + 3*settingsRowH + 56 + 92 + 190 + 104 + 20; bottom > 1448-80 {
		t.Errorf("the message sits too low (%d)", bottom)
	}
}

func TestZonePickerPagesAndChoices(t *testing.T) {
	if ZoneChoices[0].Name != "" {
		t.Fatal("the first choice is the device's own zone")
	}
	pagesN := ZonePages()
	if pagesN < 2 || pagesN*ZonesPerPage < len(ZoneChoices) || (pagesN-1)*ZonesPerPage >= len(ZoneChoices) {
		t.Fatalf("%d pages of %d for %d zones", pagesN, ZonesPerPage, len(ZoneChoices))
	}

	// Every row of every page chooses the zone drawn there; nothing below the last.
	seen := map[string]bool{}
	for p := 0; p < pagesN; p++ {
		v := SettingsView{Screen: SettingsZone, Page: p}
		for r := 0; r < ZonesPerPage; r++ {
			i := p*ZonesPerPage + r
			got := tapAt(v, SettingsRowY(r))
			if i >= len(ZoneChoices) {
				if got.Kind != ActNone {
					t.Errorf("page %d row %d is past the list but = %+v", p, r, got)
				}
				continue
			}
			if got.Kind != ActSetZone || got.Value != ZoneChoices[i].Name {
				t.Errorf("page %d row %d = %+v, want zone %q", p, r, got, ZoneChoices[i].Name)
			}
			if seen[got.Value] {
				t.Errorf("zone %q is on two rows", got.Value)
			}
			seen[got.Value] = true
		}
	}
	if len(seen) != len(ZoneChoices) {
		t.Errorf("%d zones reachable of %d", len(seen), len(ZoneChoices))
	}

	// The buttons turn pages, and not past either end.
	mid := func(r image.Rectangle) int { return (r.Min.Y + r.Max.Y) / 2 }
	next := SettingsTap(SettingsView{Screen: SettingsZone, Page: 0}, 800, mid(pickerNext), wMore)
	if next.Kind != ActZonePage || next.Page != 1 {
		t.Errorf("next on the first page = %+v", next)
	}
	prev := SettingsTap(SettingsView{Screen: SettingsZone, Page: 1}, 200, mid(pickerPrev), wMore)
	if prev.Kind != ActZonePage || prev.Page != 0 {
		t.Errorf("prev on the second page = %+v", prev)
	}
	if got := SettingsTap(SettingsView{Screen: SettingsZone, Page: 0}, 200, mid(pickerPrev), wMore); got.Kind != ActNone {
		t.Errorf("prev on the first page = %+v, want nothing", got)
	}
	if got := SettingsTap(SettingsView{Screen: SettingsZone, Page: pagesN - 1}, 800, mid(pickerNext), wMore); got.Kind != ActNone {
		t.Errorf("next on the last page = %+v, want nothing", got)
	}
	// A page number out of range (a stale view) is clamped, not a crash.
	if got := SettingsTap(SettingsView{Screen: SettingsZone, Page: 99}, 500, SettingsRowY(0), wMore); got.Kind != ActSetZone {
		t.Errorf("page 99 = %+v", got)
	}

	// The rows stop short of the buttons under them.
	if last := SettingsRowY(ZonesPerPage-1) + settingsRowH/2; last > pickerPrev.Min.Y {
		t.Errorf("the last row ends at %d, below the buttons at %d", last, pickerPrev.Min.Y)
	}
}

// A zone the picker offers that the database does not know would make a tap a
// silent failure.
func TestEveryOfferedZoneIsValid(t *testing.T) {
	for _, z := range ZoneChoices {
		if _, err := settings.ParseTimezone(z.Name); err != nil {
			t.Errorf("%q: %v", z.Name, err)
		}
	}
}

func TestZonePickerOpensOnTheChosenZonesPage(t *testing.T) {
	for i, z := range ZoneChoices {
		if got := ZonePageOf(z.Name); got != i/ZonesPerPage {
			t.Errorf("%q is on page %d, ZonePageOf says %d", z.Name, i/ZonesPerPage, got)
		}
	}
	if ZonePageOf("Antarctica/Troll") != 0 {
		t.Error("a zone that is not offered opens the first page")
	}
}

func TestZonePickerMarksTheChosenZone(t *testing.T) {
	const name = "Australia/Sydney"
	page := ZonePageOf(name)
	chosen := drawSettings(t, SettingsView{Screen: SettingsZone, Zone: name, Page: page})
	none := drawSettings(t, SettingsView{Screen: SettingsZone, Zone: "", Page: page})
	row := 0
	for i, z := range ZoneChoices {
		if z.Name == name {
			row = i - page*ZonesPerPage
		}
	}
	r := image.Rect(950, SettingsRowY(row)-36, 1072, SettingsRowY(row)+36) // the radio button
	if inked(chosen, r) <= inked(none, r) {
		t.Error("the chosen zone's radio button is not filled")
	}
}

func TestZoneLabelsAndOffsets(t *testing.T) {
	for in, want := range map[string]string{
		"": "Device", "UTC": "UTC", "Australia/Sydney": "Sydney", "America/Los_Angeles": "Los Angeles", "America/St_Johns": "St Johns",
	} {
		if got := ZoneLabel(in); got != want {
			t.Errorf("ZoneLabel(%q) = %q, want %q", in, got, want)
		}
	}
	jan := time.Date(2026, 1, 15, 0, 0, 0, 0, time.UTC)
	jul := time.Date(2026, 7, 15, 0, 0, 0, 0, time.UTC)
	for _, tc := range []struct {
		zone string
		at   time.Time
		want string
	}{
		{"UTC", jan, "UTC+0"}, {"Australia/Sydney", jan, "UTC+11"}, {"Australia/Sydney", jul, "UTC+10"},
		{"America/St_Johns", jan, "UTC-3:30"}, {"Asia/Kolkata", jul, "UTC+5:30"}, {"America/Los_Angeles", jul, "UTC-7"},
	} {
		if got := zoneOffset(tc.zone, tc.at); got != tc.want {
			t.Errorf("zoneOffset(%s, %s) = %q, want %q", tc.zone, tc.at.Format("Jan"), got, tc.want)
		}
	}
}

func TestIdleScreen(t *testing.T) {
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	IdleScreen(c, settings.DefaultIdlePath)
	if inked(c, image.Rect(0, 0, 1072, 1448)) < 5000 {
		t.Fatal("the IDLE screen is nearly empty")
	}
	// IDLE is big: it spans most of the width, and is not the NO POWER screen.
	left, right := 1072, 0
	for y := 300; y < 800; y++ {
		for x := 0; x < 1072; x++ {
			if c.Img.GrayAt(x, y).Y < 100 {
				left, right = min(left, x), max(right, x)
			}
		}
	}
	if right-left < 1072*6/10 {
		t.Errorf("IDLE spans only %d of 1072 pixels", right-left)
	}
	np, _ := render.NewCanvas(1072, 1448)
	NoPowerScreen(np)
	if !differs(c, np) {
		t.Error("the idle and no-power screens look the same")
	}
	// It names the switch: a different path draws a different picture.
	c2, _ := render.NewCanvas(1072, 1448)
	IdleScreen(c2, "electrical.switches.nav.state")
	if !differs(c, c2) {
		t.Error("the screen does not show which switch is off")
	}
	// A path too long for the width is cut short, not run off the edge.
	c3, _ := render.NewCanvas(1072, 1448)
	IdleScreen(c3, "electrical."+strings.Repeat("verylongswitchname.", 8)+"state")
	if inked(c3, image.Rect(0, 0, 20, 1448)) != 0 || inked(c3, image.Rect(1052, 0, 1072, 1448)) != 0 {
		t.Error("a long path runs to the screen's edge")
	}
}
