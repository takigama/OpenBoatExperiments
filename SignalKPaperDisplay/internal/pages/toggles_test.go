package pages

import (
	"image"
	"math"
	"testing"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func renderCompassEnv(t *testing.T, s signalk.Snapshot, e Env) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	if e.Units.Preset == "" {
		e.Units = units.Settings{Preset: units.PresetMetric}
	}
	Compass(c, s, compassNow, e)
	return c
}

// windSpeeds is the base boat with an apparent and a true wind speed (m/s).
func windSpeeds(aws, tws float64) signalk.Snapshot {
	s := base()
	if !math.IsNaN(aws) {
		s.Own.AWS = signalk.Reading{V: aws, At: compassNow}
	}
	if !math.IsNaN(tws) {
		s.Own.TWS = signalk.Reading{V: tws, At: compassNow}
	}
	return s
}

// The wind widget's name: two rows over the speed.
var (
	windRow1 = image.Rect(24, 942, 170, 992)  // "APP" / "TRU"
	windRow2 = image.Rect(24, 994, 190, 1042) // "WIND"
	windVal  = image.Rect(24, 1050, 330, 1140)
)

func TestWindWidgetNameIsTwoRowsThenTheSpeed(t *testing.T) {
	c := renderCompassEnv(t, windSpeeds(6, 5), Env{})
	for name, r := range map[string]image.Rectangle{"first row": windRow1, "second row": windRow2, "speed": windVal} {
		if inked(c, r) == 0 {
			t.Errorf("the wind widget's %s is empty", name)
		}
	}
	// The name rows are separate: a clear line between them and the speed.
	if n := inked(c, image.Rect(24, 1042, 190, 1046)); n != 0 {
		t.Errorf("the name and the speed should be apart, got %d ink between them", n)
	}
}

func TestWindWidgetSwitchesBetweenApparentAndTrue(t *testing.T) {
	s := windSpeeds(6, 9) // different speeds, so the number shows which
	app := renderCompassEnv(t, s, Env{WindTrue: false})
	tru := renderCompassEnv(t, s, Env{WindTrue: true})
	if !differs(app, tru) {
		t.Fatal("the widget should look different for true and apparent wind")
	}
	// The name row differs (APP vs TRU) and so does the number.
	if !differsIn(app, tru, windRow1) {
		t.Error("the first name row should say APP or TRU")
	}
	if !differsIn(app, tru, windVal) {
		t.Error("the speed should be the chosen wind's")
	}
	// The second row is "WIND" either way.
	if differsIn(app, tru, windRow2) {
		t.Error("the second row says WIND for both")
	}
}

func differsIn(a, b *render.Canvas, r image.Rectangle) bool { return !sameInRect(a, b, r) }

func TestWindWidgetStaysUpWhileEitherWindIsKnown(t *testing.T) {
	none := renderCompassEnv(t, windSpeeds(math.NaN(), math.NaN()), Env{})
	if inked(none, image.Rect(24, 935, 185, 1145)) != 0 { // left of where the compass ring runs
		t.Error("with no wind speed at all the widget must not be drawn")
	}
	if WindWidgetShown(windSpeeds(math.NaN(), math.NaN()).Own) {
		t.Error("WindWidgetShown with nothing")
	}
	// Only the true wind known, apparent chosen: still shown (dashes), so
	// there is something to tap to reach the true wind.
	onlyTrue := windSpeeds(math.NaN(), 7)
	app := renderCompassEnv(t, onlyTrue, Env{WindTrue: false})
	if inked(app, windVal) == 0 || inked(app, windRow1) == 0 {
		t.Error("with only the true wind known the widget should still show, as dashes")
	}
	tru := renderCompassEnv(t, onlyTrue, Env{WindTrue: true})
	if inked(tru, windVal) <= inked(app, windVal) {
		t.Errorf("the true wind's number should show more than dashes: %d vs %d", inked(tru, windVal), inked(app, windVal))
	}
	if !WindWidgetShown(onlyTrue.Own) || !WindWidgetShown(windSpeeds(3, math.NaN()).Own) {
		t.Error("either speed is enough to show the widget")
	}
}

func TestSpeedBoxLabelNamesTheSource(t *testing.T) {
	s := base()
	s.Own.STW = signalk.Reading{V: 2, At: compassNow}
	s.Own.TWD = signalk.Reading{V: s.Own.Heading.V + 1, At: compassNow}
	box := SpeedBoxRect(image.Rect(0, 0, 1072, 1448))
	sog := renderCompassEnv(t, s, Env{Speed: SpeedSOG})
	stw := renderCompassEnv(t, s, Env{Speed: SpeedSTW})
	vmg := renderCompassEnv(t, s, Env{Speed: SpeedVMG})
	label := image.Rect(box.Min.X+30, box.Min.Y+20, box.Min.X+260, box.Min.Y+100)
	for name, pair := range map[string][2]*render.Canvas{"sog/stw": {sog, stw}, "stw/vmg": {stw, vmg}, "sog/vmg": {sog, vmg}} {
		if !differsIn(pair[0], pair[1], label) {
			t.Errorf("%s: the labels should differ", name)
		}
		if !differsIn(pair[0], pair[1], box) {
			t.Errorf("%s: the boxes should differ", name)
		}
	}
}

func TestSpeedBoxShowsTheChosenSpeed(t *testing.T) {
	// A boat whose three speeds are all different.
	s := base() // SOG 3
	s.Own.STW = signalk.Reading{V: 5, At: compassNow}
	s.Own.TWD = signalk.Reading{V: s.Own.Heading.V + math.Pi/3, At: compassNow} // 60 degrees off: VMG = 5*0.5
	env := func(src SpeedSource) metric {
		return speedMetric(s.Own, compassNow, Env{Units: units.Settings{Preset: units.PresetMetric}, Speed: src})
	}
	if m := env(SpeedSOG); m.label != "SOG" || m.value != "10.8" || !m.ok { // 3 m/s in km/h
		t.Errorf("sog = %+v", m)
	}
	if m := env(SpeedSTW); m.label != "STW" || m.value != "18.0" || !m.ok {
		t.Errorf("stw = %+v", m)
	}
	if m := env(SpeedVMG); m.label != "VMG" || m.value != "9.0" || !m.ok { // 5 m/s * cos 60 = 2.5 m/s
		t.Errorf("vmg = %+v, want 9.0 km/h", m)
	}
	// Each shows dashes without its own data, rather than another speed's.
	own := s.Own
	own.STW = signalk.Reading{}
	if m := speedMetric(own, compassNow, Env{Speed: SpeedSTW}); m.ok {
		t.Error("with no speed through the water the STW box must show dashes, not SOG")
	}
	own.TWD = signalk.Reading{}
	if m := speedMetric(own, compassNow, Env{Speed: SpeedVMG}); m.ok {
		t.Error("with no true wind the VMG box must show dashes")
	}
	// A label that changes in place has to be solid black.
	if !env(SpeedSTW).liveLabel {
		t.Error("the speed box's label changes while the screen is up, so it must be drawn solid")
	}
}

func TestSpeedSourceCyclesAndStartsAsSOG(t *testing.T) {
	var s SpeedSource
	if s != SpeedSOG || s.Label() != "SOG" {
		t.Errorf("the zero value should be SOG, got %v %q", s, s.Label())
	}
	seen := []string{s.Label()}
	for i := 0; i < 3; i++ {
		s = s.Next()
		seen = append(seen, s.Label())
	}
	want := []string{"SOG", "STW", "VMG", "SOG"}
	for i := range want {
		if seen[i] != want[i] {
			t.Fatalf("cycle = %v, want %v", seen, want)
		}
	}
}

func TestSpeedLabelIsSolidBlack(t *testing.T) {
	c := renderCompassEnv(t, base(), Env{Speed: SpeedSTW})
	box := SpeedBoxRect(c.Bounds())
	label := image.Rect(box.Min.X+30, box.Min.Y+20, box.Min.X+260, box.Min.Y+100)
	darkest := uint8(255)
	for y := label.Min.Y; y < label.Max.Y; y++ {
		for x := label.Min.X; x < label.Max.X; x++ {
			darkest = min(darkest, c.Img.GrayAt(x, y).Y)
		}
	}
	if darkest > 10 {
		t.Errorf("the speed label is drawn in grey %d; it is redrawn in place and must be black", darkest)
	}
}

func TestWindAndSpeedTapTargetsAreOnScreenAndSeparate(t *testing.T) {
	b := image.Rect(0, 0, 1072, 1448)
	w, s := WindWidgetRect(b), SpeedBoxRect(b)
	if w.Overlaps(s) {
		t.Errorf("the wind widget %v and the speed box %v overlap", w, s)
	}
	if !w.In(b) || !s.In(b) {
		t.Error("tap targets must be on the screen")
	}
	// Both are in the left third - which is why they have to win over "previous page".
	if w.Max.X > 1072/2 || s.Max.X > 1072/2 {
		t.Error("expected both targets on the left half")
	}
	// The wind widget's text is inside its target.
	if !windRow1.In(w) || !windVal.In(w) {
		t.Errorf("the widget's text %v %v is outside its target %v", windRow1, windVal, w)
	}
}

func TestNavClosestAISSaysClosingOrOpening(t *testing.T) {
	closing := navBoat(1) // we are steaming at a stationary ship ahead
	opening := navBoat(1)
	opening.Own.Heading = signalk.Reading{V: math.Pi, At: compassNow}
	opening.Own.COG = signalk.Reading{V: math.Pi, At: compassNow} // turned round: moving away
	cell := navCell(image.Rect(0, 0, 1072, 1448), 5)
	word := image.Rect(cell.Min.X+200, cell.Max.Y-58, cell.Max.X-200, cell.Max.Y-8)
	a, b := renderNav(t, closing), renderNav(t, opening)
	if inked(a, word) == 0 || inked(b, word) == 0 {
		t.Fatalf("the CLOSING / OPENING word is missing: %d, %d ink", inked(a, word), inked(b, word))
	}
	if !differsIn(a, b, word) {
		t.Error("CLOSING and OPENING should read differently")
	}
	// No diamond any more at the right of the label line.
	if inked(a, image.Rect(cell.Max.X-100, cell.Min.Y+30, cell.Max.X-10, cell.Min.Y+90)) != 0 {
		t.Error("the diamond should be gone from the label line")
	}
	// With no contact there is no word.
	none := renderNav(t, navBoat(0))
	if inked(none, word) != 0 {
		t.Error("with no contact there is nothing to call closing or opening")
	}
}
