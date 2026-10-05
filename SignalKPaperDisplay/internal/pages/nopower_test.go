package pages

import (
	"image"
	"testing"

	"signalkpaperdisplay/internal/render"
)

func TestNoPowerLabels(t *testing.T) {
	for min, want := range map[int]string{
		0: "Never", 1: "1 minute", 5: "5 minutes", 30: "30 minutes", 60: "1 hour", 120: "2 hours", 480: "8 hours",
		// Not on the list, from the web API.
		2: "2 minutes", 45: "45 minutes", 90: "90 minutes", 180: "3 hours", 10080: "168 hours", -5: "Never",
	} {
		if got := NoPowerLabel(min); got != want {
			t.Errorf("NoPowerLabel(%d) = %q, want %q", min, got, want)
		}
	}
	// One minute to never, in order, never last.
	if NoPowerChoices[0].Minutes != 1 || NoPowerChoices[len(NoPowerChoices)-1].Minutes != 0 {
		t.Errorf("the choices run from 1 minute to never: %+v", NoPowerChoices)
	}
	for i := 1; i < len(NoPowerChoices)-1; i++ {
		if NoPowerChoices[i].Minutes <= NoPowerChoices[i-1].Minutes {
			t.Errorf("choices out of order at %d", i)
		}
	}
	hasHour := false
	for _, c := range NoPowerChoices {
		hasHour = hasHour || c.Minutes == 60
	}
	if !hasHour {
		t.Error("the default, an hour, must be one of the choices")
	}
}

func noPowerCanvas(t *testing.T, w, h int) *render.Canvas {
	t.Helper()
	var c *render.Canvas
	var err error
	if w == 1072 {
		c, err = render.NewCanvas(w, h)
	} else {
		c, err = render.NewScaledCanvas(w, h, DesignWidth)
	}
	if err != nil {
		t.Fatal(err)
	}
	NoPowerScreen(c)
	return c
}

// bands lists the runs of rows that have any ink, as [first, last] pairs.
func bands(c *render.Canvas) [][2]int {
	b := c.Img.Bounds()
	var out [][2]int
	in := false
	for y := b.Min.Y; y < b.Max.Y; y++ {
		any := false
		for x := b.Min.X; x < b.Max.X && !any; x++ {
			any = c.Img.GrayAt(x, y).Y < 128
		}
		switch {
		case any && !in:
			out = append(out, [2]int{y, y})
			in = true
		case any:
			out[len(out)-1][1] = y
		default:
			in = false
		}
	}
	return out
}

func TestNoPowerScreenIsBigAndOnTwoLines(t *testing.T) {
	c := noPowerCanvas(t, 1072, 1448)
	bs := bands(c)
	if len(bs) != 3 {
		t.Fatalf("want NO, POWER and a small line underneath: %d bands %v", len(bs), bs)
	}
	no, power, small := bs[0], bs[1], bs[2]
	for _, big := range [][2]int{no, power} {
		if h := big[1] - big[0]; h < 150 {
			t.Errorf("a line of the message is only %d px tall: it should be huge", h)
		}
	}
	if small[1]-small[0] > 80 {
		t.Errorf("the line underneath is as big as the message: %v", small)
	}
	// POWER spans nearly the whole width.
	minX, maxX := 1<<30, -1
	for y := power[0]; y <= power[1]; y++ {
		for x := 0; x < 1072; x++ {
			if c.Img.GrayAt(x, y).Y < 128 {
				minX, maxX = min(minX, x), max(maxX, x)
			}
		}
	}
	if maxX-minX < 1072*80/100 {
		t.Errorf("POWER spans %d of 1072 px: it should fill the width", maxX-minX)
	}
	if minX < 20 || maxX > 1072-20 {
		t.Errorf("POWER touches the edge: %d..%d", minX, maxX)
	}
	// NO is centred over it.
	nx0, nx1 := 1<<30, -1
	for y := no[0]; y <= no[1]; y++ {
		for x := 0; x < 1072; x++ {
			if c.Img.GrayAt(x, y).Y < 128 {
				nx0, nx1 = min(nx0, x), max(nx1, x)
			}
		}
	}
	if mid := (nx0 + nx1) / 2; mid < 520 || mid > 552 {
		t.Errorf("NO is centred at %d, not 536", mid)
	}
	// The whole message sits in the middle of the screen, not at an edge.
	if no[0] < 300 || small[1] > 1150 {
		t.Errorf("the message is not in the middle: %v .. %v", no, small)
	}
}

func TestNoPowerScreenIsTheSameOnASmallScreen(t *testing.T) {
	big := noPowerCanvas(t, 1072, 1448)
	small := noPowerCanvas(t, 600, 800)
	if got := correlation(blockDarkness(downsample(big.Img, 600, 800), 20, 20), blockDarkness(small.Img, 20, 20)); got < 0.85 {
		t.Errorf("on a 600x800 screen the message does not match: correlation %.2f", got)
	}
	// Still huge there.
	bs := bands(small)
	if len(bs) < 2 || bs[1][1]-bs[1][0] < 80 {
		t.Errorf("on a small screen the bands are %v", bs)
	}
}

func TestPowerScreenOffersTheTimeoutRow(t *testing.T) {
	const w = 1072
	row := noPowerRowRect(w)
	for i := range PowerChoices {
		if row.Overlaps(powerButton(i, w)) {
			t.Errorf("the timeout row overlaps the %s button", PowerChoices[i].ID)
		}
	}
	got := SettingsTap(SettingsView{Screen: SettingsPower}, mid(row).X, mid(row).Y, w)
	if got.Kind != ActOpenNoPower {
		t.Errorf("a tap on the row = %+v", got)
	}
	// Its words change with the setting.
	one := drawSettings(t, SettingsView{Screen: SettingsPower, NoPower: 60})
	never := drawSettings(t, SettingsView{Screen: SettingsPower, NoPower: 0})
	if !differs(one, never) {
		t.Error("the row should say the current timeout")
	}
	if inked(one, row) == 0 {
		t.Error("the row is not drawn")
	}
	if row.Max.Y > 1300 {
		t.Errorf("the row is too low for a small screen: %v", row)
	}
	// It opens the picker, and back goes up to the power screen.
	if ParentScreen(SettingsNoPower) != SettingsPower {
		t.Error("back from the picker should reach the power screen")
	}
}

func TestNoPowerPicker(t *testing.T) {
	const w = 1072
	v := SettingsView{Screen: SettingsNoPower, NoPower: 60}
	for i, ch := range NoPowerChoices {
		got := SettingsTap(v, 500, SettingsRowY(i), w)
		if got.Kind != ActSetNoPower || got.Level != ch.Minutes {
			t.Errorf("row %d (%s) = %+v", i, ch.Label, got)
		}
	}
	if got := SettingsTap(v, 500, SettingsRowY(len(NoPowerChoices)), w); got.Kind != ActNone {
		t.Errorf("a tap below the list = %+v", got)
	}
	if got := SettingsTap(v, 30, 40, w); got.Kind != ActBack {
		t.Errorf("the chevron = %+v", got)
	}
	// The radio for the current choice is the one filled in.
	rowAt := func(min int) image.Rectangle {
		for i, c := range NoPowerChoices {
			if c.Minutes == min {
				y := SettingsRowY(i)
				return image.Rect(930, y-30, 1030, y+30)
			}
		}
		t.Fatalf("no choice for %d", min)
		return image.Rectangle{}
	}
	hour := drawSettings(t, v)
	never := drawSettings(t, SettingsView{Screen: SettingsNoPower, NoPower: 0})
	if inked(hour, rowAt(60)) <= inked(never, rowAt(60)) {
		t.Error("the current choice should have its radio filled")
	}
	if inked(never, rowAt(0)) <= inked(hour, rowAt(0)) {
		t.Error("Never should be filled when it is the choice")
	}
	// The list and its note fit on the smallest screen.
	if bottom := settingsTop + len(NoPowerChoices)*settingsRowH + 60 + 2*46; bottom > 1300 {
		t.Errorf("the note runs off a small screen: %d", bottom)
	}
}
