package app

import (
	"fmt"
	"log"
	"time"

	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

// Everything in this file is what can be changed from outside the touch screen -
// the web page (internal/web) is the one thing that does. Each change does what
// the same change from the touch screen does: the same state, the same saving
// (or not saving: the wind and speed widgets always start the same way on every
// boot), the same kind of redraw.

// Control is a snapshot of everything that can be controlled, and a few facts
// that go with it.
type Control struct {
	Page     string   // the ID of the page showing
	Pages    []Choice // every page
	Invert   bool
	Boxes    []string // the Nav page's boxes, as kind IDs
	LightMax int      // 0: this device has no front light
	Light    int
	WindTrue bool   // the compass wind widget shows the true wind
	Speed    string // the compass speed widget's kind ID
	Depth    string // the compass depth widget's kind ID
	// The map page: its range in nautical miles, whether north is up, and the
	// ranges there are.
	MapRange   int
	MapNorthUp bool
	MapRanges  []int
	Demo       bool
	Server     string
	// DefaultServer is the server used when none was chosen: the launcher's.
	DefaultServer string
	Units         units.Settings
	Connected     bool
}

// Choice is an ID and the name to show for it.
type Choice struct{ ID, Name string }

// Control reports the current state.
func (a *App) Control() Control {
	a.mu.Lock()
	c := Control{
		Page:          pages.All()[a.page].ID,
		Invert:        a.Invert,
		Boxes:         pages.NormalizeBoxes(a.Boxes),
		WindTrue:      a.windTrue,
		Speed:         a.speed.ID(),
		Depth:         pages.DepthWidgetID(a.depthW),
		MapRange:      pages.MapRangeOrDefault(a.mapRange),
		MapNorthUp:    a.mapNorthUp,
		MapRanges:     append([]int(nil), pages.MapRanges...),
		Demo:          a.Demo,
		Server:        a.serverLocked(),
		DefaultServer: a.DefaultServer,
		Units:         a.Units.Clone(),
		Light:         a.lightLevel,
	}
	a.mu.Unlock()
	for _, p := range pages.All() {
		c.Pages = append(c.Pages, Choice{p.ID, p.Title})
	}
	if a.Light != nil {
		c.LightMax = a.Light.Max()
	} else {
		c.Light = 0
	}
	if a.State != nil {
		c.Connected = a.State.Snapshot().Connected
	}
	return c
}

// Paths lists the numeric SignalK paths the server has sent, for picking one to
// show.
func (a *App) Paths() []signalk.PathInfo {
	if a.State == nil {
		return nil
	}
	return a.State.Catalog()
}

// settingsFileLocked is what gets saved. The caller holds a.mu.
func (a *App) settingsFileLocked() settings.File {
	f := settings.File{Settings: a.Units.Clone(), Invert: a.Invert, Boxes: append([]string(nil), a.Boxes...), Server: a.Server}
	if a.Brightness != nil {
		b := *a.Brightness
		f.Brightness = &b
	}
	return f
}

func (a *App) saveSettings(f settings.File) {
	if a.SettingsPath == "" {
		return
	}
	if err := settings.Save(a.SettingsPath, f); err != nil {
		log.Printf("settings: could not save %s: %v", a.SettingsPath, err)
	}
}

// ChoosePage shows the page with that ID.
func (a *App) ChoosePage(id string) error {
	if !a.SetPage(id) {
		return fmt.Errorf("no page %q", id)
	}
	log.Printf("remote: page is now %s", id)
	a.nudge()
	return nil
}

// SetInvert switches white-on-black on or off, and saves it.
func (a *App) SetInvert(on bool) {
	a.mu.Lock()
	changed := a.Invert != on
	a.Invert = on
	a.pageChanged = true // every pixel changes: a full refresh, as for the touch switch
	f := a.settingsFileLocked()
	a.mu.Unlock()
	if changed {
		log.Printf("remote: invert is now %v", on)
		a.saveSettings(f)
	}
	a.nudge()
}

// ValidBox reports whether a kind ID may be put in a Nav box: one of the built-in
// kinds, or a raw SignalK path.
func ValidBox(id string) bool {
	_, ok := pages.BoxKindByID(id)
	return ok
}

// SetBox makes Nav box i (0-based) show a kind, and saves it.
func (a *App) SetBox(i int, kind string) error {
	if i < 0 || i >= pages.NavBoxes {
		return fmt.Errorf("there is no box %d (0 to %d)", i, pages.NavBoxes-1)
	}
	if !ValidBox(kind) {
		return fmt.Errorf("a box cannot show %q", kind)
	}
	a.mu.Lock()
	a.Boxes = pages.NormalizeBoxes(a.Boxes)
	a.Boxes[i] = kind
	a.pageChanged = true
	f := a.settingsFileLocked()
	a.mu.Unlock()
	log.Printf("remote: Nav box %d now shows %s", i+1, kind)
	a.saveSettings(f)
	a.syncWatch()
	a.nudge()
	return nil
}

// SetBrightness sets the front light, and saves the level.
func (a *App) SetBrightness(level int) error {
	if a.Light == nil {
		return fmt.Errorf("this device has no front light")
	}
	if level < 0 || level > a.Light.Max() {
		return fmt.Errorf("brightness is 0 to %d", a.Light.Max())
	}
	if err := a.Light.Set(level); err != nil {
		return fmt.Errorf("front light: %w", err)
	}
	a.mu.Lock()
	a.lightLevel = level
	chosen := level
	a.Brightness = &chosen
	f := a.settingsFileLocked()
	a.mu.Unlock()
	log.Printf("remote: backlight is now %d", level)
	a.saveSettings(f)
	a.nudge()
	return nil
}

// SetWindTrue makes the compass wind widget show the true wind (or the
// apparent). Not saved: it starts as apparent on every boot.
func (a *App) SetWindTrue(on bool) {
	a.mu.Lock()
	a.windTrue = on
	a.force = true
	a.mu.Unlock()
	log.Printf("remote: wind speed widget shows %s", map[bool]string{false: "apparent", true: "true"}[on])
	a.nudge()
}

// SetSpeed makes the compass speed widget show a kind - a speed, any other
// single-number kind a Nav box can show, or a raw SignalK path. Not saved: it
// starts as SOG on every boot.
func (a *App) SetSpeed(kind string) error {
	src, ok := pages.SpeedFromID(kind)
	if !ok {
		return fmt.Errorf("the speed widget cannot show %q", kind)
	}
	a.mu.Lock()
	a.speed = src
	a.force = true
	a.mu.Unlock()
	log.Printf("remote: speed widget shows %s", src.ID())
	a.syncWatch()
	a.nudge()
	return nil
}

// SetDepth makes the compass depth widget show a kind, as the speed widget can.
// Not saved: it shows depth on every boot. A full refresh, since the widget's
// label is a static one.
func (a *App) SetDepth(kind string) error {
	setting, ok := pages.DepthFromID(kind)
	if !ok {
		return fmt.Errorf("the depth widget cannot show %q", kind)
	}
	a.mu.Lock()
	a.depthW = setting
	a.pageChanged = true
	a.mu.Unlock()
	log.Printf("remote: depth widget shows %s", pages.DepthWidgetID(setting))
	a.syncWatch()
	a.nudge()
	return nil
}

// SetMapRange sets the map's range, in nautical miles (one of pages.MapRanges).
// Not saved: it is 5 on every boot.
func (a *App) SetMapRange(nm int) error {
	if !pages.MapRangeOK(nm) {
		return fmt.Errorf("the map has no %d nm range (%v)", nm, pages.MapRanges)
	}
	a.mu.Lock()
	a.mapRange = nm
	if nm == pages.DefaultMapRange {
		a.mapRange = 0
	}
	a.force = true
	a.mu.Unlock()
	log.Printf("remote: map range is now %d nm", nm)
	a.nudge()
	return nil
}

// SetMapNorthUp puts north up on the map, or our heading. Not saved.
func (a *App) SetMapNorthUp(on bool) {
	a.mu.Lock()
	a.mapNorthUp = on
	a.force = true
	a.mu.Unlock()
	log.Printf("remote: map is %s", map[bool]string{false: "heading up", true: "north up"}[on])
	a.nudge()
}

// SyncWatch tells the state which SignalK paths the boxes and the speed widget
// need, and fetches their units from the server. Call it once after setting the
// boxes up; the setters above call it themselves.
func (a *App) SyncWatch() { a.syncWatch() }

func (a *App) syncWatch() {
	if a.State == nil {
		return
	}
	a.mu.Lock()
	ids := append(pages.NormalizeBoxes(a.Boxes), a.speed.ID(), pages.DepthWidgetID(a.depthW))
	a.mu.Unlock()
	a.State.Watch(pages.WatchedPaths(ids...))
	a.refreshMeta()
}

// metaRetry is how long to wait before asking the server about a path's units
// again after a failure (the server may simply not be up yet).
const metaRetry = time.Minute

// refreshMeta asks the server, in the background, for the units of every watched
// path it has not yet answered for.
func (a *App) refreshMeta() {
	if a.FetchMeta == nil || a.State == nil {
		return
	}
	a.mu.Lock()
	ids := append(pages.NormalizeBoxes(a.Boxes), a.speed.ID(), pages.DepthWidgetID(a.depthW))
	if a.metaTried == nil {
		a.metaTried = map[string]time.Time{}
	}
	var ask []string
	now := time.Now()
	for _, p := range pages.WatchedPaths(ids...) {
		if a.State.HasMeta(p) || now.Sub(a.metaTried[p]) < metaRetry {
			continue
		}
		a.metaTried[p] = now
		ask = append(ask, p)
	}
	a.mu.Unlock()
	for _, p := range ask {
		go a.FetchMeta(p)
	}
}
