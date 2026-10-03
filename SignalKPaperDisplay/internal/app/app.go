// Package app ties the pieces together: it renders the current page from the
// live SignalK state and pushes frames to whatever Display it was given.
package app

import (
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"log"
	"math"
	"sync"
	"time"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/frontlight"
	"signalkpaperdisplay/internal/input"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/settings"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

type App struct {
	State    *signalk.State
	Display  display.Display
	Interval time.Duration
	// FullRefreshEvery forces a flashing full-panel refresh this often, to
	// clear the ghosting that accumulates from partial updates. Zero means
	// only the first frame is a full refresh.
	FullRefreshEvery time.Duration
	// MinRefresh is the shortest gap between partial refreshes; zero means
	// redraw whenever the picture changes.
	MinRefresh time.Duration
	// Units are the user's unit choices. They change from the touch handler
	// while the renderer reads them, so inside the app go through unitsNow()
	// and the settings handler, never touch the field directly once running.
	Units units.Settings
	// Invert shows white on black instead of black on white. Applied as the
	// last step of every frame, so it covers every page with no per-page work.
	Invert bool
	// HeaderGuardEvery is how often the header strip is repainted whatever
	// the app thinks is on screen, to clear out anything the stock UI has
	// drawn over it (see guardHeader). Zero turns the guard off.
	HeaderGuardEvery time.Duration
	// Boxes is what each of the Nav page's six boxes shows (page box-kind IDs;
	// short or unknown entries take their defaults). Like Units, it changes
	// from the touch handler, so inside the app use boxesNow().
	Boxes []string
	// Server is the SignalK server chosen in settings, as "host:port"; empty
	// means none was chosen and DefaultServer (the -signalk flag) is used.
	Server        string
	DefaultServer string
	// OnServerChange is called, with the new "host:port", after the user saves
	// a different server, so the connection can be moved to it.
	OnServerChange func(hostPort string)
	// Demo is whether demo mode is on: made-up data standing in for the server's.
	// It is not saved, so a restart always goes back to the real server. Set it
	// before running to start in demo mode; after that the settings screen
	// changes it, and OnDemoChange is told each time it does (not for the
	// starting value, which is the caller's to act on). Guarded by mu.
	Demo         bool
	OnDemoChange func(on bool)
	// OnPower carries out a choice from the power screen (one of the pages.Power*
	// IDs: back to the Kindle's own software, restart, power off) once it has
	// been confirmed, after the screen has been left saying what is happening.
	// Nil means this display has no power to manage (a PC preview), and the
	// choices do nothing. If it fails, the dashboard carries on.
	OnPower func(kind string) error
	// FetchMeta asks the SignalK server which units a path is in and records the
	// answer in State (see signalk.Client.FetchMeta); it is called in its own
	// goroutine. Nil means don't ask: paths are then formatted from their names.
	FetchMeta func(path string)
	metaTried map[string]time.Time // when each path's units were last asked for; guarded by mu
	// Light is the device's front light, nil if it has none or it can't be
	// reached; the setting is hidden then. Brightness is the level saved in
	// settings (nil: never chosen). Call InitLight once after setting both.
	Light      frontlight.Light
	Brightness *int
	lightLevel int // the level the light is at, guarded by mu

	// Battery reads the device's own battery for the header; nil when there is
	// none to read. The reading is cached (see batteryNow).
	Battery   battery.Reader
	battStat  *battery.Status
	battAt    time.Time
	battErr   string
	battEvery time.Duration

	// What the compass page's two tappable widgets show: the wind speed (true or
	// apparent) and the speed (SOG, STW, VMG). Not saved - they start as apparent
	// wind and SOG on every boot. Guarded by mu.
	windTrue bool
	speed    pages.SpeedSource
	// force asks for a redraw at once, past the partial-refresh rationing, for a
	// tap that changes a value on the same screen (not a new page).
	force bool

	// Verbose logs every screen refresh. Off, only slow ones are logged: one
	// line every couple of seconds is a megabyte a day on a device with little
	// room to spare.
	Verbose bool
	// SettingsPath is where settings changes are saved; empty means don't persist.
	SettingsPath string

	showMu   sync.Mutex // one picture sent to the display at a time
	farewell string     // the power choice being carried out; its last picture replaces every page. Guarded by mu.

	mu           sync.Mutex // guards the fields below, which touch input changes
	page         int
	pageChanged  bool
	settingsOpen bool
	settingsView pages.SettingsView
	wake         chan struct{} // nudges Run to redraw now, not at the next tick

	renderTook       time.Duration // last frame's page rendering, only touched by Show
	lastHeartbeatErr string        // only touched by the main loop
	lastGuard        time.Time     // header guard state, only touched by the main loop
	guardMinute      int64
	lastGuardErr     string
}

func (a *App) nudge() {
	a.mu.Lock()
	if a.wake == nil {
		a.wake = make(chan struct{}, 1)
	}
	w := a.wake
	a.mu.Unlock()
	select {
	case w <- struct{}{}:
	default: // a redraw is already pending
	}
}

func (a *App) wakeChan() <-chan struct{} {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.wake == nil {
		a.wake = make(chan struct{}, 1)
	}
	return a.wake
}

// PrevPage cycles to the preceding page, wrapping around.
func (a *App) PrevPage() {
	n := len(pages.All())
	a.mu.Lock()
	a.page = (a.page + n - 1) % n
	a.pageChanged = true
	a.mu.Unlock()
}

// batteryNow is the device battery for the header, read at most every
// battEvery (30 s unless set): the reading is a few file reads or a lipc call,
// and the charge changes slowly. A failed read hides the indicator rather than
// showing a stale one. Called only from the rendering goroutine.
func (a *App) batteryNow(now time.Time) *battery.Status {
	if a.Battery == nil {
		return nil
	}
	every := a.battEvery
	if every == 0 {
		every = 30 * time.Second
	}
	if !a.battAt.IsZero() && now.Sub(a.battAt) < every {
		return a.battStat
	}
	a.battAt = now
	st, err := a.Battery.Read()
	if err != nil {
		a.battStat = nil
		if err.Error() != a.battErr {
			a.battErr = err.Error()
			log.Printf("battery: %v", err)
		}
		return nil
	}
	a.battErr = ""
	// Say when it goes on or off the charger, with what the device reported: the
	// log is how to tell, from outside, what the indicator was working from.
	if prev := a.battStat; prev == nil || prev.Plugged != st.Plugged || prev.Charging != st.Charging {
		log.Printf("battery: %d%%, plugged in %v, charging %v", st.Percent, st.Plugged, st.Charging)
	}
	a.battStat = &st
	return a.battStat
}

// InitLight applies the saved brightness, if any, and otherwise reads where
// the light is, so the setting starts out showing the truth.
func (a *App) InitLight() {
	if a.Light == nil {
		return
	}
	level, err := a.Light.Level()
	if b := a.Brightness; b != nil && *b >= 0 && *b <= a.Light.Max() {
		level = *b
		err = a.Light.Set(level)
	} else if b != nil {
		// Saved on a different scale (v30 first offered the raw 0-4095): not
		// a level any more, so leave the light as the device has it.
		log.Printf("front light: ignoring saved brightness %d, outside 0-%d", *b, a.Light.Max())
		a.Brightness = nil
	}
	if err != nil {
		log.Printf("front light: %v", err)
	}
	a.mu.Lock()
	a.lightLevel = level
	a.mu.Unlock()
}

// withLight fills in the front light's state on a settings view, which is how
// the (pure) settings screens get to know it.
func (a *App) withLight(v pages.SettingsView) pages.SettingsView {
	a.mu.Lock()
	v.Demo = a.Demo
	v.Level = a.lightLevel
	a.mu.Unlock()
	if a.Light != nil {
		v.MaxLevel = a.Light.Max()
	} else {
		v.Level = 0
	}
	return v
}

// OpenSettings shows a settings screen directly, for previews.
func (a *App) OpenSettings(v pages.SettingsView) {
	a.mu.Lock()
	a.settingsOpen, a.settingsView, a.pageChanged = true, v, true
	a.mu.Unlock()
}

// unitsNow returns a private copy of the current unit settings.
func (a *App) unitsNow() units.Settings {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.Units.Clone()
}

// HandleEvent reacts to a touch gesture. On a normal page, tapping the cog
// opens settings, tapping the right third (or swiping left) goes to the next
// page, and the left third (or swiping right) to the previous one. The
// middle is deliberately inert, so a stray tap there does nothing. While
// settings are open every tap belongs to the settings screens.
func (a *App) HandleEvent(ev input.Event) {
	// Touches arrive in device pixels; every tap area is in design units.
	ev.X, ev.Y = a.toDesign(ev.X, ev.Y)
	a.mu.Lock()
	open := a.settingsOpen
	a.mu.Unlock()
	if open {
		if ev.Kind == input.Tap {
			a.handleSettingsTap(ev)
		}
		return
	}
	if ev.Kind == input.Tap && image.Pt(ev.X, ev.Y).In(pages.CogRect) {
		a.mu.Lock()
		a.settingsOpen, a.settingsView, a.pageChanged = true, pages.SettingsView{}, true
		a.mu.Unlock()
		log.Printf("touch: tap at (%d,%d) -> settings", ev.X, ev.Y)
		a.nudge()
		return
	}

	w, h := a.designSize()
	if ev.Kind == input.Tap && a.compassTap(image.Pt(ev.X, ev.Y), image.Rect(0, 0, w, h)) {
		return
	}
	action := ""
	switch {
	case ev.Kind == input.SwipeLeft, ev.Kind == input.Tap && ev.X > w*2/3:
		a.NextPage()
		action = "next page"
	case ev.Kind == input.SwipeRight, ev.Kind == input.Tap && ev.X < w/3:
		a.PrevPage()
		action = "previous page"
	}
	// Logged either way: on an e-ink screen the redraw lags, so the log is
	// the only immediate proof that a touch was recognised at all.
	if action == "" {
		log.Printf("touch: %s at (%d,%d) - no action there", ev.Kind, ev.X, ev.Y)
		return
	}
	log.Printf("touch: %s at (%d,%d) -> %s (%s)", ev.Kind, ev.X, ev.Y, action, a.currentPage().ID)
	a.nudge()
}

// compassTap handles a tap on one of the compass page's two tappable widgets,
// reporting whether it was on one. They sit in the left third of the screen,
// where a tap would otherwise mean "previous page", so these win there.
func (a *App) compassTap(pt image.Point, b image.Rectangle) bool {
	if a.currentPage().ID != "compass" {
		return false
	}
	switch {
	case pt.In(pages.WindWidgetRect(b)) && pages.WindWidgetShown(a.State.Snapshot().Own):
		a.mu.Lock()
		a.windTrue = !a.windTrue
		now := a.windTrue
		a.force = true
		a.mu.Unlock()
		log.Printf("touch: tap at (%d,%d) -> wind speed shows %s", pt.X, pt.Y, map[bool]string{false: "apparent", true: "true"}[now])
	case pt.In(pages.SpeedBoxRect(b)):
		a.mu.Lock()
		a.speed = a.speed.Next()
		label := a.speed.Label()
		a.force = true
		a.mu.Unlock()
		log.Printf("touch: tap at (%d,%d) -> speed shows %s", pt.X, pt.Y, label)
		a.syncWatch()
	default:
		return false
	}
	a.nudge()
	return true
}

// designScale is how many device pixels there are to a design unit.
func (a *App) designScale() float64 {
	w, _ := a.Display.Size()
	return float64(w) / float64(pages.DesignWidth)
}

// designSize is the screen's size in design units, which is what the pages
// and their tap areas are in.
func (a *App) designSize() (w, h int) {
	_, dh := a.Display.Size()
	return pages.DesignWidth, int(math.Round(float64(dh) / a.designScale()))
}

// toDesign converts a point in device pixels to design units.
func (a *App) toDesign(x, y int) (int, int) {
	s := a.designScale()
	if s == 1 {
		return x, y
	}
	return int(math.Round(float64(x) / s)), int(math.Round(float64(y) / s))
}

// takeForce reports, and clears, whether a redraw was asked for past the
// rationing.
func (a *App) takeForce() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	f := a.force
	a.force = false
	return f
}

// SetPage switches to the page with the given ID, reporting whether it exists.
func (a *App) SetPage(id string) bool {
	for i, p := range pages.All() {
		if p.ID == id {
			a.mu.Lock()
			a.page, a.pageChanged = i, true
			a.mu.Unlock()
			return true
		}
	}
	return false
}

// NextPage cycles to the following page, wrapping around.
func (a *App) NextPage() {
	a.mu.Lock()
	a.page = (a.page + 1) % len(pages.All())
	a.pageChanged = true
	a.mu.Unlock()
}

func (a *App) currentPage() pages.Page {
	a.mu.Lock()
	defer a.mu.Unlock()
	return pages.All()[a.page]
}

// takePageChanged reports (and clears) whether the page switched since the
// last frame. A different page is a completely different picture, so it's
// shown with a full refresh rather than ghosting over the old one.
func (a *App) takePageChanged() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	changed := a.pageChanged
	a.pageChanged = false
	return changed
}

// handleSettingsTap applies one tap on a settings screen.
func (a *App) handleSettingsTap(ev input.Event) {
	a.mu.Lock()
	view := a.settingsView
	a.mu.Unlock()
	view = a.withLight(view)

	act := pages.SettingsTap(view, ev.X, ev.Y, pages.DesignWidth)
	if act.Kind == pages.ActNone {
		return
	}

	a.mu.Lock()
	var err error
	var newServer string // set when a different server was saved
	setLight := -1       // set when the front light is to change
	powerKind := ""      // set when a power choice was confirmed
	demoChanged := false // set when demo mode was switched
	switch act.Kind {
	case pages.ActBack:
		if view.Screen == pages.SettingsRoot {
			a.settingsOpen = false
		} else {
			a.settingsView = pages.SettingsView{Screen: pages.ParentScreen(view.Screen)}
		}
	case pages.ActOpenPresetPicker:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPickPreset}
	case pages.ActOpenUnitPicker:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPickUnit, Metric: act.Metric}
	case pages.ActSetPreset:
		err = a.Units.SetPreset(act.Value)
		a.settingsView = pages.SettingsView{}
	case pages.ActSetUnit:
		err = a.Units.SetUnit(act.Metric, act.Value)
		a.settingsView = pages.SettingsView{}
	case pages.ActToggleInvert:
		a.Invert = !a.Invert // stays on the list: you see the result at once
	case pages.ActOpenBoxes:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsBoxes}
	case pages.ActOpenBoxPicker:
		// Open on the page holding the box's current choice.
		cur := pages.NormalizeBoxes(a.Boxes)[act.Box]
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPickBox, Box: act.Box, Page: pages.BoxPageOf(cur)}
	case pages.ActBoxPage:
		a.settingsView.Page = act.Page
	case pages.ActOpenLight:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsLight}
	case pages.ActSetLight:
		setLight = act.Level
		a.lightLevel = act.Level
		chosen := act.Level
		a.Brightness = &chosen
	case pages.ActOpenPower:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPower}
	case pages.ActPowerPick:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsPowerConfirm, Power: act.Value}
	case pages.ActPowerDo:
		powerKind = act.Value
	case pages.ActToggleDemo:
		a.Demo = !a.Demo // stays on the list; not saved, so it never outlives the app
		demoChanged = true
	case pages.ActOpenServer:
		a.settingsView = pages.SettingsView{Screen: pages.SettingsServer, Text: a.serverLocked()}
	case pages.ActServerKey:
		switch act.Value {
		case "save":
			norm, perr := settings.NormalizeServer(view.Text)
			if perr != nil {
				a.settingsView.Err = perr.Error()
				break
			}
			if norm != a.serverLocked() {
				a.Server = norm
				newServer = norm
			}
			a.settingsView = pages.SettingsView{}
		default:
			a.settingsView.Text = pages.ApplyServerKey(a.settingsView.Text, act.Value)
			a.settingsView.Err = ""
		}
	case pages.ActSetBox:
		a.Boxes = pages.NormalizeBoxes(a.Boxes)
		a.Boxes[act.Box] = act.Value
		a.settingsView = pages.SettingsView{Screen: pages.SettingsBoxes}
	}
	changed := act.Kind == pages.ActSetPreset || act.Kind == pages.ActSetUnit ||
		act.Kind == pages.ActToggleInvert || act.Kind == pages.ActSetBox || newServer != "" || setLight >= 0
	saved := a.settingsFileLocked()
	// Every screen is a different picture, so a change of screen is a full
	// refresh - but moving the light is the same screen redrawn, and a flash
	// on every tap would make it miserable to use.
	if act.Kind != pages.ActSetLight {
		a.pageChanged = true
	}
	a.mu.Unlock()

	if err != nil {
		log.Printf("settings: %v", err)
	}
	if act.Kind == pages.ActSetBox {
		a.syncWatch()
	}
	if powerKind != "" {
		a.doPower(powerKind)
	}
	if demoChanged {
		a.mu.Lock()
		on := a.Demo
		a.mu.Unlock()
		log.Printf("settings: demo mode is now %v", map[bool]string{true: "on", false: "off"}[on])
		if a.OnDemoChange != nil {
			a.OnDemoChange(on)
		}
	}
	if changed && a.SettingsPath != "" {
		if err := settings.Save(a.SettingsPath, saved); err != nil {
			log.Printf("settings: could not save %s: %v", a.SettingsPath, err)
		}
	}
	if setLight >= 0 && a.Light != nil {
		if err := a.Light.Set(setLight); err != nil {
			log.Printf("front light: %v", err)
		}
	}
	if newServer != "" {
		log.Printf("settings: SignalK server is now %s", newServer)
		if a.OnServerChange != nil {
			a.OnServerChange(newServer)
		}
	}
	log.Printf("touch: settings tap at (%d,%d)", ev.X, ev.Y)
	a.nudge()
}

// PowerButton is a press of the Kindle's power button. The dashboard owns the
// screen and the stock software that would normally answer the button is
// stopped, so it opens the power screen from wherever the app is; pressed again
// there, it closes it.
func (a *App) PowerButton() {
	a.mu.Lock()
	if a.farewell != "" { // already going down: nothing more to ask
		a.mu.Unlock()
		return
	}
	if a.settingsOpen && (a.settingsView.Screen == pages.SettingsPower || a.settingsView.Screen == pages.SettingsPowerConfirm) {
		a.settingsOpen = false
	} else {
		a.settingsOpen, a.settingsView = true, pages.SettingsView{Screen: pages.SettingsPower}
	}
	a.pageChanged = true
	a.mu.Unlock()
	log.Print("power button: pressed")
	a.nudge()
}

// doPower carries out a confirmed power choice. The last picture is drawn first,
// in full: the e-ink screen keeps it after the Kindle has gone off, and it is
// what says what happened. If the action fails the dashboard comes back.
func (a *App) doPower(kind string) {
	if a.OnPower == nil {
		log.Printf("power: %s (nothing to do on this display)", kind)
		return
	}
	a.mu.Lock()
	a.farewell = kind
	a.settingsOpen = false
	a.mu.Unlock()
	log.Printf("power: %s", kind)
	if _, err := a.Show(time.Now(), true); err != nil {
		log.Printf("power: drawing the last picture: %v", err)
	}
	if err := a.OnPower(kind); err != nil {
		log.Printf("power: %s failed: %v", kind, err)
		a.mu.Lock()
		a.farewell = ""
		a.settingsOpen, a.settingsView, a.pageChanged = true, pages.SettingsView{Screen: pages.SettingsPower}, true
		a.mu.Unlock()
		a.nudge()
	}
}

// heartbeat blinks the dot beside the clock, once per loop, by redrawing just
// its small rectangle. It runs from the main loop on purpose: if the app
// hangs, the dot stops, which is the point of having it. It does nothing on
// displays that can't update a region, and on the settings screens, which
// have no header clock to sit beside.
func (a *App) heartbeat(now time.Time) {
	rd, ok := a.Display.(display.RegionDisplay)
	if !ok {
		return
	}
	a.mu.Lock()
	open := a.settingsOpen
	a.mu.Unlock()
	if open {
		return
	}
	w, _ := a.Display.Size()
	img, rect := pages.HeartbeatImage(w, pages.HeartbeatOn(now), pages.Lost(a.State.Snapshot(), now))
	if img == nil {
		return
	}
	if a.invertNow() {
		img = invertedCopy(img)
	}
	err := rd.ShowRegion(img, rect.Min.X, rect.Min.Y)
	// Once a second would flood the log if it fails, so say it once per distinct error.
	if err != nil && !errors.Is(err, display.ErrNoRegion) && err.Error() != a.lastHeartbeatErr {
		a.lastHeartbeatErr = err.Error()
		log.Printf("heartbeat: %v", err)
	}
}

// Frame renders the current page (or the settings screens) for the given moment.
func (a *App) Frame(now time.Time) (*image.Gray, error) {
	w, h := a.Display.Size()
	c, err := render.NewScaledCanvas(w, h, pages.DesignWidth)
	if err != nil {
		return nil, err
	}
	a.mu.Lock()
	open, view := a.settingsOpen, a.settingsView
	a.mu.Unlock()
	view = a.withLight(view)
	u := a.unitsNow()
	invert := a.invertNow()
	boxes := a.boxesNow()
	server := a.serverNow()
	a.mu.Lock()
	windTrue, speed, demo, farewell := a.windTrue, a.speed, a.Demo, a.farewell
	a.mu.Unlock()
	if farewell != "" {
		pages.Farewell(c, farewell)
	} else if open {
		pages.Settings(c, view, u, invert, boxes, server)
	} else {
		a.currentPage().Draw(c, a.State.Snapshot(), now, pages.Env{Units: u, Boxes: boxes, Battery: a.batteryNow(now), WindTrue: windTrue, Speed: speed, Demo: demo})
	}
	if invert {
		invertInPlace(c.Img)
	}
	return c.Img, nil
}

// serverLocked is the SignalK server in use: the one chosen in settings, else
// the default. The caller holds a.mu.
func (a *App) serverLocked() string {
	if a.Server != "" {
		return a.Server
	}
	return a.DefaultServer
}

func (a *App) serverNow() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.serverLocked()
}

// boxesNow returns a private, normalised copy of the Nav box layout.
func (a *App) boxesNow() []string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return pages.NormalizeBoxes(a.Boxes)
}

func (a *App) invertNow() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.Invert
}

// invertInPlace flips every pixel: black becomes white and the reverse, with
// greys mirrored. Done once at the end of a frame rather than by every page.
func invertInPlace(img *image.Gray) {
	for i, v := range img.Pix {
		img.Pix[i] = 255 - v
	}
}

// invertedCopy is invertInPlace on a copy, for images that share pixels with
// another (the heartbeat's region is a window into a larger canvas).
func invertedCopy(src *image.Gray) *image.Gray {
	b := src.Bounds()
	dst := image.NewGray(b)
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			dst.SetGray(x, y, color.Gray{Y: 255 - src.GrayAt(x, y).Y})
		}
	}
	return dst
}

func (a *App) Show(now time.Time, full bool) (drew bool, err error) {
	a.showMu.Lock()
	defer a.showMu.Unlock()
	t0 := time.Now()
	img, err := a.Frame(now)
	if err != nil {
		return false, err
	}
	a.renderTook = time.Since(t0)
	return a.Display.Show(img, full)
}

// timing describes where the last frame's time went: our own page
// rendering, plus the display's breakdown if it offers one.
func (a *App) timing() string {
	s := fmt.Sprintf("render %s", a.renderTook.Round(time.Millisecond))
	if t, ok := a.Display.(interface{ LastTiming() string }); ok {
		s += ", " + t.LastTiming()
	}
	return s
}

// slowRefresh is how long a partial refresh may take before it is logged
// even when not verbose.
const slowRefresh = 1200 * time.Millisecond

// Run redraws every Interval until ctx is cancelled.
func (a *App) Run(ctx context.Context) {
	t := time.NewTicker(a.Interval)
	defer t.Stop()
	var lastFull, lastDraw time.Time // zero, so the first frame is a full refresh
	for {
		now := time.Now()
		full := a.takePageChanged() || lastFull.IsZero() ||
			(a.FullRefreshEvery > 0 && now.Sub(lastFull) >= a.FullRefreshEvery)
		forced := a.takeForce()
		// Partial refreshes are rationed: a slow panel (eips takes ~3.5s a
		// frame) redrawing on every tick would be busy nearly all the time,
		// so a tap's page change would queue behind it. Full refreshes
		// (page changes, the periodic flash) always go straight through.
		if full || forced || a.MinRefresh <= 0 || now.Sub(lastDraw) >= a.MinRefresh {
			started := time.Now()
			drew, err := a.Show(now, full)
			took := time.Since(started)
			if drew {
				lastDraw = started
			}
			switch {
			case err != nil:
				log.Printf("display: %v", err)
			case full:
				lastFull = now
				log.Printf("display: full refresh (%s) took %s (%s)", a.currentPage().ID, took.Round(time.Millisecond), a.timing())
			case drew && (a.Verbose || took > slowRefresh):
				log.Printf("display: partial refresh took %s (%s)", took.Round(time.Millisecond), a.timing())
			}
		}
		a.heartbeat(time.Now())
		a.guardHeader(time.Now())
		a.refreshMeta() // cheap: only asks about a path it has not had an answer for in the last minute
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		case <-a.wakeChan():
		}
	}
}
