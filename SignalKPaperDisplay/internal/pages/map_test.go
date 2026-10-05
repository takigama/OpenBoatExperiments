package pages

import (
	"image"
	"math"
	"testing"
	"time"

	"signalkpaperdisplay/internal/ais"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

var mapNow = time.Date(2026, 10, 5, 12, 0, 0, 0, time.UTC)

// mapOwn is our boat: at a position, heading 0 (north) at 3 m/s, all fresh.
func mapOwn(heading float64) signalk.Own {
	f := func(v float64) signalk.Reading { return signalk.Reading{V: v, At: mapNow} }
	return signalk.Own{
		Pos:     signalk.Position{Lat: -33.85, Lon: 151.28, At: mapNow},
		Heading: f(heading), COG: f(heading), SOG: f(3),
	}
}

// ship is a target at a bearing (degrees true) and range (metres) from mapOwn's
// position, moving on a course (degrees) at a speed (m/s); speed < 0 sends no motion.
func ship(id string, bearingDeg, rangeM, courseDeg, speed float64) signalk.Target {
	b := bearingDeg * math.Pi / 180
	own := mapOwn(0).Pos
	dn := math.Cos(b) * rangeM
	de := math.Sin(b) * rangeM
	t := signalk.Target{
		ID: "urn:mrn:imo:mmsi:" + id, MMSI: id, Name: "SHIP" + id,
		Pos: signalk.Position{
			Lat: own.Lat + dn/111320.0,
			Lon: own.Lon + de/(111320.0*math.Cos(own.Lat*math.Pi/180)),
			At:  mapNow,
		},
	}
	if speed >= 0 {
		t.COG = signalk.Reading{V: courseDeg * math.Pi / 180, At: mapNow}
		t.SOG = signalk.Reading{V: speed, At: mapNow}
	}
	return t
}

func mapSnap(own signalk.Own, targets ...signalk.Target) signalk.Snapshot {
	return signalk.Snapshot{Own: own, Targets: targets, Connected: true, LastMessage: mapNow}
}

func renderMap(t *testing.T, s signalk.Snapshot, e Env) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	if e.Units.Preset == "" {
		e.Units = units.Settings{Preset: units.PresetNautical}
	}
	Map(c, s, mapNow, e)
	return c
}

func TestMapRanges(t *testing.T) {
	if DefaultMapRange != 5 || !MapRangeOK(5) || MapRangeOK(3) || MapRangeOK(0) {
		t.Error("5 is the default and one of the ranges; 3 and 0 are not")
	}
	if MapRangeOrDefault(0) != 5 || MapRangeOrDefault(2) != 2 {
		t.Error("zero means the default")
	}
	// A tap goes round 1, 2, 5, 10 and back, starting from the default.
	got := []int{}
	nm := 0
	for i := 0; i < 5; i++ {
		nm = NextMapRange(nm)
		got = append(got, nm)
	}
	want := []int{10, 1, 2, 5, 10}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("taps from the default go %v, want %v", got, want)
		}
	}
	if NextMapRange(7) != DefaultMapRange {
		t.Error("an unknown range goes to the default")
	}
}

func TestMapOffsetPutsShipsWhereTheyAre(t *testing.T) {
	const R, rangeM = 400.0, 9260.0 // 5 nm
	near := func(a, b float64) bool { return math.Abs(a-b) < 1e-6 }
	// A ship due north at half range, our heading north, heading-up: straight up, half way out.
	dx, dy := mapOffset(ais.Contact{Bearing: 0, Range: rangeM / 2}, 0, rangeM, R)
	if !near(dx, 0) || !near(dy, -R/2) {
		t.Errorf("north, heading-up: %v, %v", dx, dy)
	}
	// Due east, north-up: to the right.
	dx, dy = mapOffset(ais.Contact{Bearing: math.Pi / 2, Range: rangeM}, 0, rangeM, R)
	if !near(dx, R) || !near(dy, 0) {
		t.Errorf("east, north-up: %v, %v", dx, dy)
	}
	// Heading east, heading-up: a ship due east is dead ahead, so straight up.
	dx, dy = mapOffset(ais.Contact{Bearing: math.Pi / 2, Range: rangeM}, math.Pi/2, rangeM, R)
	if !near(dx, 0) || !near(dy, -R) {
		t.Errorf("east, heading east: %v, %v", dx, dy)
	}
	// Heading east: a ship due north is on our port side, to the left.
	dx, dy = mapOffset(ais.Contact{Bearing: 0, Range: rangeM}, math.Pi/2, rangeM, R)
	if !near(dx, -R) || !near(dy, 0) {
		t.Errorf("north, heading east: %v, %v", dx, dy)
	}
}

func TestRangeText(t *testing.T) {
	for _, tc := range []struct {
		m    float64
		u    units.Settings
		want string
	}{
		{5 * 1852, units.Settings{Preset: units.PresetNautical}, "5 nm"},
		{2.5 * 1852, units.Settings{Preset: units.PresetNautical}, "2.5 nm"},
		{1852, units.Settings{Preset: units.PresetNautical}, "1 nm"},
		{10 * 1852, units.Settings{Preset: units.PresetNautical}, "10 nm"},
		{5 * 1852, units.Settings{Preset: units.PresetMetric}, "9.26 km"},
		{5 * 1852, units.Settings{Preset: units.PresetImperial}, "5.75 mi"},
	} {
		if got := rangeText(tc.m, tc.u); got != tc.want {
			t.Errorf("rangeText(%v, %s) = %q, want %q", tc.m, tc.u.Preset, got, tc.want)
		}
	}
}

// shipCentre is where a ship should be drawn on the 1072x1448 canvas.
func shipCentre(rangeM, bearingRad, up float64, nm int) (int, int) {
	cx, cy, r, _ := mapLayout(image.Rect(0, 0, 1072, 1448))
	k := ais.Contact{Bearing: bearingRad, Range: rangeM}
	dx, dy := mapOffset(k, up, float64(nm)*nauticalMile, r)
	return int(math.Round(cx + dx)), int(math.Round(cy + dy))
}

func dark(c *render.Canvas, x, y int) bool { return c.Img.GrayAt(x, y).Y < 100 }

func TestMapDrawsOurBoatAtTheCentreAndTheRings(t *testing.T) {
	c := renderMap(t, mapSnap(mapOwn(0)), Env{})
	cx, cy, r, _ := mapLayout(image.Rect(0, 0, 1072, 1448))
	if !dark(c, int(cx), int(cy)+8) {
		t.Error("our boat is not at the centre")
	}
	// The outer ring and the dashed half ring.
	if !dark(c, int(cx), int(cy-r)+1) && !dark(c, int(cx), int(cy-r)) && !dark(c, int(cx), int(cy-r)-1) {
		t.Error("no outer ring at the top")
	}
	if inked(c, image.Rect(int(cx-r/2)-6, int(cy)-60, int(cx-r/2)+6, int(cy)+60)) == 0 {
		t.Error("no half-range ring")
	}
	// Everything outside the ring is clear of ships, and the corner labels are there.
	if inked(c, image.Rect(20, headerH+8, 260, headerH+90)) == 0 {
		t.Error("no HDG UP label")
	}
	if inked(c, image.Rect(800, headerH+8, 1050, headerH+90)) == 0 {
		t.Error("no range in the corner")
	}
}

func TestMapShowsShipsInRangeAndOnlyThose(t *testing.T) {
	// Due east, 2.5 nm out: half way along the right-hand radius.
	east := ship("1", 90, 2.5*nauticalMile, 0, 4)
	with := renderMap(t, mapSnap(mapOwn(0), east), Env{MapNorthUp: true})
	without := renderMap(t, mapSnap(mapOwn(0)), Env{MapNorthUp: true})
	x, y := shipCentre(2.5*nauticalMile, math.Pi/2, 0, 5)
	if !differs(with, without) {
		t.Fatal("a ship in range changed nothing")
	}
	if inked(with, image.Rect(x-26, y-26, x+26, y+26)) <= inked(without, image.Rect(x-26, y-26, x+26, y+26))+20 {
		t.Errorf("no ship where it should be, (%d,%d)", x, y)
	}

	// 8 nm out is beyond a 5 nm map: not drawn at all, nor named.
	far := ship("2", 90, 8*nauticalMile, 0, 4)
	if differs(renderMap(t, mapSnap(mapOwn(0), far), Env{}), renderMap(t, mapSnap(mapOwn(0)), Env{})) {
		// the strip's count is the same (none), so the whole picture must be the same
		t.Error("a ship beyond the range changed the picture")
	}
	// But at 10 nm it is there.
	if !differs(renderMap(t, mapSnap(mapOwn(0), far), Env{MapRange: 10}), renderMap(t, mapSnap(mapOwn(0)), Env{MapRange: 10})) {
		t.Error("a ship within a 10 nm range was not drawn")
	}
}

func TestMapSymbolsSayWhetherAShipIsClosing(t *testing.T) {
	// Due east of us, same distance both times: one steering at us (closing), one
	// steering away (opening). Our boat is stopped so only the ship moves.
	own := mapOwn(0)
	own.SOG = signalk.Reading{V: 0, At: mapNow}
	closing := ship("1", 90, 2*nauticalMile, 270, 4) // course west: at us
	opening := ship("2", 90, 2*nauticalMile, 90, 4)  // course east: away
	cs := ais.Contacts(own, []signalk.Target{closing}, mapNow, StaleAfter)
	os := ais.Contacts(own, []signalk.Target{opening}, mapNow, StaleAfter)
	if len(cs) != 1 || !cs[0].Closing || len(os) != 1 || os[0].Closing {
		t.Fatalf("test setup: closing=%+v opening=%+v", cs, os)
	}
	x, y := shipCentre(2*nauticalMile, math.Pi/2, 0, 5)
	e := Env{MapNorthUp: true}
	cl := renderMap(t, mapSnap(own, closing), e)
	op := renderMap(t, mapSnap(own, opening), e)
	if !dark(cl, x, y) {
		t.Error("a ship getting nearer should be a solid symbol")
	}
	if dark(op, x, y) {
		t.Error("a ship getting further should be hollow")
	}
	if inked(op, image.Rect(x-30, y-30, x+30, y+30)) == 0 {
		t.Error("the hollow one has no outline")
	}
}

func TestMapSymbolPointsAlongTheShipsCourse(t *testing.T) {
	// A ship due east, heading north (course 0) versus heading south (course 180):
	// the pictures differ, and each leans the way it should - the tip is the
	// pointed end, so most of the ink is on that side of the centre.
	own := mapOwn(0)
	own.SOG = signalk.Reading{V: 0, At: mapNow}
	e := Env{MapNorthUp: true}
	x, y := shipCentre(3*nauticalMile, math.Pi/2, 0, 5)
	north := renderMap(t, mapSnap(own, ship("1", 90, 3*nauticalMile, 0, 0.05)), e)
	south := renderMap(t, mapSnap(own, ship("1", 90, 3*nauticalMile, 180, 0.05)), e)
	// The symbol is a dart: narrow at the tip, wide at the back. Compare the ink in
	// the outer thirds of its height.
	narrowEnd := func(c *render.Canvas) string {
		minY, maxY := 1<<30, -1
		for yy := y - 45; yy <= y+45; yy++ {
			for xx := x - 20; xx <= x+20; xx++ {
				if dark(c, xx, yy) {
					minY, maxY = min(minY, yy), max(maxY, yy)
				}
			}
		}
		if maxY < 0 {
			return "none"
		}
		third := (maxY - minY + 1) / 3
		count := func(y0, y1 int) int {
			n := 0
			for yy := y0; yy < y1; yy++ {
				for xx := x - 20; xx <= x+20; xx++ {
					if dark(c, xx, yy) {
						n++
					}
				}
			}
			return n
		}
		top, bottom := count(minY, minY+third), count(maxY-third+1, maxY+1)
		switch {
		case top < bottom:
			return "up"
		case bottom < top:
			return "down"
		}
		return "same"
	}
	if got := narrowEnd(north); got != "up" {
		t.Errorf("a ship heading north should have its tip up, the narrow end is %s", got)
	}
	if got := narrowEnd(south); got != "down" {
		t.Errorf("a ship heading south should have its tip down, the narrow end is %s", got)
	}
}

func TestMapOrientation(t *testing.T) {
	// We head east; a ship is due east, so dead ahead.
	own := mapOwn(math.Pi / 2)
	s := mapSnap(own, ship("1", 90, 3*nauticalMile, 90, 4))
	headingUp := renderMap(t, s, Env{})
	northUp := renderMap(t, s, Env{MapNorthUp: true})
	hx, hy := shipCentre(3*nauticalMile, math.Pi/2, math.Pi/2, 5) // straight up
	nx, ny := shipCentre(3*nauticalMile, math.Pi/2, 0, 5)         // off to the right
	if hx == nx && hy == ny {
		t.Fatal("test setup: the two places are the same")
	}
	box := func(x, y int) image.Rectangle { return image.Rect(x-30, y-30, x+30, y+30) }
	if inked(headingUp, box(hx, hy)) <= inked(northUp, box(hx, hy)) {
		t.Error("heading-up: the ship ahead should be straight up the screen")
	}
	if inked(northUp, box(nx, ny)) <= inked(headingUp, box(nx, ny)) {
		t.Error("north-up: a ship due east should be to the right")
	}
	// With no heading there is nothing to point up, so it is north-up whatever was asked.
	noHeading := mapSnap(own, ship("1", 90, 3*nauticalMile, 90, 4))
	noHeading.Own.Heading = signalk.Reading{}
	if inked(renderMap(t, noHeading, Env{}), box(nx, ny)) == 0 {
		t.Error("with no heading the map should be north-up")
	}
}

func TestMapSaysWhatIsMissing(t *testing.T) {
	cx, cy, r, _ := mapLayout(image.Rect(0, 0, 1072, 1448))
	noPos := mapOwn(0)
	noPos.Pos = signalk.Position{}
	c := renderMap(t, mapSnap(noPos, ship("1", 120, 4*nauticalMile, 0, 4)), Env{})
	if inked(c, image.Rect(int(cx)-150, int(cy+r*0.55)-40, int(cx)+150, int(cy+r*0.55)+14)) == 0 {
		t.Error("with no position the map should say so")
	}
	x, y := shipCentre(4*nauticalMile, 120*math.Pi/180, 0, 5)
	if inked(c, image.Rect(x-30, y-30, x+30, y+30)) != 0 {
		t.Error("with no position no ship can be placed on the map")
	}
	// Nothing at all: still a map, with our boat on it and an empty count.
	empty := renderMap(t, mapSnap(mapOwn(0)), Env{})
	if !dark(empty, int(cx), int(cy)+8) {
		t.Error("an empty map still has our boat")
	}
}

func TestMapStripNamesTheShipThatWillPassClosest(t *testing.T) {
	own := mapOwn(0)
	own.SOG = signalk.Reading{V: 0, At: mapNow}
	strip := image.Rect(560, 1220, 1060, 1440)
	none := renderMap(t, mapSnap(own, ship("1", 90, 2*nauticalMile, 90, 4)), Env{})  // heading away: no CPA
	conv := renderMap(t, mapSnap(own, ship("1", 90, 2*nauticalMile, 270, 4)), Env{}) // coming at us
	if !differs(none, conv) {
		t.Fatal("a converging ship changed nothing")
	}
	if inked(conv, strip) <= inked(none, strip)-200 && !differsIn(none, conv, strip) {
		t.Error("the strip should say which ship will pass closest")
	}
	// And the one that will pass closest is ringed on the map: more ink around it.
	x, y := shipCentre(2*nauticalMile, math.Pi/2, 0, 5)
	ring := image.Rect(x-60, y-60, x+60, y+60)
	if inked(conv, ring) <= inked(none, ring) {
		t.Error("the ship that will pass closest should have a ring round it")
	}
}

func TestMapWindMarkersAreOutsideTheRing(t *testing.T) {
	own := mapOwn(0)
	own.AWA = signalk.Reading{V: 0.2, At: mapNow}
	with := renderMap(t, mapSnap(own), Env{})
	own.AWA = signalk.Reading{}
	without := renderMap(t, mapSnap(own), Env{})
	cx, cy, r, _ := mapLayout(image.Rect(0, 0, 1072, 1448))
	if !differs(with, without) {
		t.Fatal("a live apparent wind angle should draw a marker")
	}
	// They sit outside the ring: inside it, nothing changed.
	inside := image.Rect(int(cx-r)+20, int(cy-r)+20, int(cx+r)-20, int(cy+r)-20)
	if differsIn(with, without, inside) {
		t.Error("the wind marker has strayed into the map")
	}
	// A stale wind draws nothing.
	own.AWA = signalk.Reading{V: 0.2, At: mapNow.Add(-time.Minute)}
	if differs(renderMap(t, mapSnap(own), Env{}), without) {
		t.Error("a stale wind angle should not be drawn")
	}
}

func TestMapHasNoGreyFills(t *testing.T) {
	// Ships move and the page is redrawn in place under a waveform that has no grey,
	// so nothing on the map may be a grey fill. The soft edge of a drawn line or
	// shape is one pixel wide and fine; a fill is wider than that, so look for a
	// 3x3 block of grey.
	own := mapOwn(0)
	own.AWA = signalk.Reading{V: 0.4, At: mapNow}
	s := mapSnap(own, ship("1", 90, 2*nauticalMile, 270, 4), ship("2", 200, 3*nauticalMile, 20, 5), ship("3", 330, 0.4*nauticalMile, 150, 2))
	grey := func(c *render.Canvas, x, y int) bool { v := c.Img.GrayAt(x, y).Y; return v > 40 && v < 215 }
	for _, e := range []Env{{}, {MapNorthUp: true}, {MapRange: 1}, {MapRange: 10}} {
		c := renderMap(t, s, e)
		cx, cy, r, _ := mapLayout(image.Rect(0, 0, 1072, 1448))
		for y := int(cy-r) + 1; y < int(cy+r)-1; y++ {
			for x := int(cx-r) + 1; x < int(cx+r)-1; x++ {
				all := true
				for dy := -1; dy <= 1 && all; dy++ {
					for dx := -1; dx <= 1; dx++ {
						all = all && grey(c, x+dx, y+dy)
					}
				}
				if all {
					t.Fatalf("env %+v: a grey fill at %d,%d: it would not survive the fast waveform", e, x, y)
				}
			}
		}
	}
}

func TestMapIsTheSameLayoutOnASmallScreen(t *testing.T) {
	own := mapOwn(0.3)
	own.AWA = signalk.Reading{V: 0.4, At: mapNow}
	s := mapSnap(own, ship("1", 80, 2*nauticalMile, 250, 4), ship("2", 200, 3*nauticalMile, 20, 5))
	e := Env{Units: units.Settings{Preset: units.PresetNautical}}

	big := image.NewGray(image.Rect(0, 0, 1072, 1448))
	bc, _ := render.NewCanvas(1072, 1448)
	Map(bc, s, mapNow, e)
	copy(big.Pix, bc.Img.Pix)

	sc, err := render.NewScaledCanvas(600, 800, DesignWidth)
	if err != nil {
		t.Fatal(err)
	}
	Map(sc, s, mapNow, e)
	ref := downsample(big, 600, 800)
	if got := correlation(blockDarkness(ref, 20, 20), blockDarkness(sc.Img, 20, 20)); got < 0.9 {
		t.Errorf("the map on a 600x800 screen does not match the big one scaled down: correlation %.2f", got)
	}
}

func TestMapHitAreasAreWhereTheyAreDrawn(t *testing.T) {
	b := image.Rect(0, 0, 1072, 1448)
	o, rg, w := MapOrientRect(b), MapRangeRect(b), MapWindRect(b)
	if o.Overlaps(rg) || o.Overlaps(w) || rg.Overlaps(w) {
		t.Errorf("the three tap areas overlap: %v %v %v", o, rg, w)
	}
	// Each contains its text.
	if !image.Pt(100, headerH+4+40).In(o) || !image.Pt(1000, headerH+4+40).In(rg) || !image.Pt(100, 1380).In(w) {
		t.Error("a tap area misses its text")
	}
	// None reaches the cog, which opens settings.
	for _, r := range []image.Rectangle{o, rg, w} {
		if r.Overlaps(CogRect) {
			t.Errorf("%v overlaps the settings cog", r)
		}
	}
	// The map itself is not a button.
	cx, cy, _, _ := mapLayout(b)
	for _, r := range []image.Rectangle{o, rg, w} {
		if image.Pt(int(cx), int(cy)).In(r) {
			t.Errorf("the centre of the map is inside %v: a stray tap on it would do something", r)
		}
	}
}
