package pages

import (
	"fmt"
	"image"
	"strconv"
	"strings"
	"time"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/settings"
)

// The "more settings" screen - the time zone, idle mode and software update - and
// the screens under it. They sit behind one row of the main list because that list
// already fills the screen.

// The more screen's rows.
const (
	moreZoneRow   = 0
	moreIdleRow   = 1
	moreGPSRow    = 2
	moreUpdateRow = 3
)

// NoGPSChoice is one GPS timeout the settings offer, in seconds (0 is off).
type NoGPSChoice struct {
	Seconds int
	Label   string
}

// NoGPSChoices are the timeouts on the settings screen, shortest first and off last.
// The web API accepts any number of seconds from settings.MinNoGPSSeconds to
// settings.MaxNoGPSSeconds, or 0.
var NoGPSChoices = []NoGPSChoice{
	{5, "5 seconds"}, {10, "10 seconds"}, {30, "30 seconds"}, {60, "1 minute"}, {300, "5 minutes"}, {0, "Off"},
}

// NoGPSLabel names a GPS timeout in seconds, whether or not it is one of the choices.
func NoGPSLabel(seconds int) string {
	for _, c := range NoGPSChoices {
		if c.Seconds == seconds {
			return c.Label
		}
	}
	switch {
	case seconds <= 0:
		return "Off"
	case seconds%60 == 0 && seconds/60 == 1:
		return "1 minute"
	case seconds%60 == 0:
		return fmt.Sprintf("%d minutes", seconds/60)
	}
	return fmt.Sprintf("%d seconds", seconds)
}

// ZoneChoice is one time zone offered on the device. The web page takes any name the
// zone database knows; the screen has room for a short list of the places boats go.
type ZoneChoice struct{ Name string }

// ZoneChoices are the zones on the picker, "" (the device's own) first and then west
// to east. A name is an IANA one; the database is built into the program.
var ZoneChoices = []ZoneChoice{
	{""}, {"UTC"},
	{"Pacific/Honolulu"}, {"America/Anchorage"}, {"America/Los_Angeles"}, {"America/Denver"},
	{"America/Chicago"}, {"America/New_York"}, {"America/Halifax"}, {"America/St_Johns"},
	{"America/Sao_Paulo"}, {"Atlantic/Azores"}, {"Europe/London"}, {"Europe/Paris"},
	{"Europe/Athens"}, {"Africa/Johannesburg"}, {"Asia/Dubai"}, {"Indian/Mauritius"},
	{"Asia/Kolkata"}, {"Asia/Bangkok"}, {"Asia/Singapore"}, {"Asia/Hong_Kong"},
	{"Asia/Tokyo"}, {"Australia/Perth"}, {"Australia/Darwin"}, {"Australia/Adelaide"},
	{"Australia/Brisbane"}, {"Australia/Sydney"}, {"Australia/Hobart"}, {"Pacific/Noumea"},
	{"Pacific/Auckland"}, {"Pacific/Fiji"},
}

// ZonesPerPage is how many zones one page of the picker holds.
const ZonesPerPage = 12

// ZonePages is how many pages the picker has.
func ZonePages() int { return (len(ZoneChoices) + ZonesPerPage - 1) / ZonesPerPage }

// ZonePageOf is the page of the picker a zone is on, so it opens where the choice
// in force can be seen; the first page if the zone is not one of the choices.
func ZonePageOf(name string) int {
	for i, z := range ZoneChoices {
		if z.Name == name {
			return i / ZonesPerPage
		}
	}
	return 0
}

// ZoneLabel names a zone for the screen: "Sydney" for Australia/Sydney, "Device" for
// the device's own.
func ZoneLabel(name string) string {
	switch name {
	case "":
		return "Device"
	case "UTC":
		return "UTC"
	}
	if i := strings.LastIndex(name, "/"); i >= 0 {
		name = name[i+1:]
	}
	return strings.ReplaceAll(name, "_", " ")
}

// zoneOffset says how far a zone is from UTC right now: "UTC+10", "UTC-3:30".
func zoneOffset(name string, now time.Time) string {
	loc, err := settings.ParseTimezone(name)
	if err != nil {
		return ""
	}
	if loc == nil {
		loc = time.Local
	}
	_, off := now.In(loc).Zone()
	sign := "+"
	if off < 0 {
		sign, off = "-", -off
	}
	h, m := off/3600, off%3600/60
	if m == 0 {
		return fmt.Sprintf("UTC%s%d", sign, h)
	}
	return fmt.Sprintf("UTC%s%d:%02d", sign, h, m)
}

// zoneTap is what a tap does on the time zone picker: turn the page, or choose.
func zoneTap(v SettingsView, pt image.Point) Action {
	pages := ZonePages()
	page := max(0, min(v.Page, pages-1))
	if pt.In(pickerPrev) && page > 0 {
		return Action{Kind: ActZonePage, Page: page - 1}
	}
	if pt.In(pickerNext) && page < pages-1 {
		return Action{Kind: ActZonePage, Page: page + 1}
	}
	if r := rowAt(pt.Y); r >= 0 && r < ZonesPerPage {
		if i := page*ZonesPerPage + r; i < len(ZoneChoices) {
			return Action{Kind: ActSetZone, Value: ZoneChoices[i].Name}
		}
	}
	return Action{}
}

// drawZones draws one page of the picker, with the zone in use marked.
func drawZones(c *render.Canvas, v SettingsView) {
	b := c.Bounds()
	now := time.Now()
	pages := ZonePages()
	page := max(0, min(v.Page, pages-1))
	for j := 0; j < ZonesPerPage; j++ {
		i := page*ZonesPerPage + j
		if i >= len(ZoneChoices) {
			break
		}
		name := ZoneChoices[i].Name
		label := ZoneLabel(name)
		if name == "" {
			label = "Device default"
		}
		row(c, j, label, func(cy int) {
			c.Text(b.Dx()-130, cy+14, zoneOffset(name, now), 38, render.Regular, render.Right, render.Dark)
			radio(c, b.Dx()-70, cy, name == v.Zone)
		})
	}
	button := func(r image.Rectangle, label string, on bool) {
		c.FillRect(r, render.Black)
		c.FillRect(r.Inset(4), render.White)
		shade := render.Black
		if !on {
			shade = render.Mid // nowhere to go that way
		}
		c.Text((r.Min.X+r.Max.X)/2, (r.Min.Y+r.Max.Y)/2+18, label, 52, render.Bold, render.Center, shade)
	}
	button(pickerPrev, "< PREV", page > 0)
	button(pickerNext, "NEXT >", page < pages-1)
	c.Text(b.Dx()/2, (pickerPrev.Min.Y+pickerPrev.Max.Y)/2+16, strconv.Itoa(page+1)+" / "+strconv.Itoa(pages), 44, render.Bold, render.Center, render.Black)
}

// drawMore draws the more screen. What changes while it is up (the clock, the
// update's progress) is solid black, as anything redrawn in place has to be.
func drawMore(c *render.Canvas, v SettingsView) {
	b := c.Bounds()
	row(c, moreZoneRow, "Time zone", func(cy int) {
		c.Text(b.Dx()-110, cy+16, ZoneLabel(v.Zone), 44, render.Bold, render.Right, render.Black)
		chevronRight(c, b.Dx()-50, cy, 18, render.Dark)
	})
	row(c, moreIdleRow, "Idle switch", func(cy int) {
		state := "Off"
		if v.IdleOn {
			state = "On"
		}
		c.Text(b.Dx()-50, cy+16, state, 50, render.Bold, render.Right, render.Black)
	})
	row(c, moreGPSRow, "NO DATA after", func(cy int) {
		c.Text(b.Dx()-110, cy+16, NoGPSLabel(v.NoGPS), 44, render.Bold, render.Right, render.Black)
		chevronRight(c, b.Dx()-50, cy, 18, render.Dark)
	})
	row(c, moreUpdateRow, "Software update", func(cy int) {
		right := "v" + Version
		if v.UpdateBusy {
			right = "Working..."
		}
		c.Text(b.Dx()-50, cy+16, right, 44, render.Bold, render.Right, render.Black)
	})

	y := settingsTop + 4*settingsRowH + 56
	if v.Clock != "" {
		c.Text(40, y, "The clock reads "+v.Clock+" in this zone.", 38, render.Regular, render.Left, render.Black)
	}
	y += 92
	c.Text(40, y, "Idle mode puts the screen to sleep while this", 36, render.Regular, render.Left, render.Dark)
	c.Text(40, y+44, "SignalK switch is off (set it on the web page):", 36, render.Regular, render.Left, render.Dark)
	c.Text(40, y+96, fitText(c, v.IdlePath, 40, render.Bold, b.Dx()-80), 40, render.Bold, render.Left, render.Black)
	y += 190
	if v.NoGPS <= 0 {
		c.Text(40, y, "NO DATA does not watch our own GPS (it still shows", 36, render.Regular, render.Left, render.Dark)
		c.Text(40, y+44, "if the link drops or nothing arrives at all).", 36, render.Regular, render.Left, render.Black)
	} else {
		c.Text(40, y, "NO DATA shows when our own GPS position has not", 36, render.Regular, render.Left, render.Dark)
		c.Text(40, y+44, fitText(c, "updated for "+NoGPSLabel(v.NoGPS)+" (a fix is about 1 a second).", 36, render.Regular, b.Dx()-80), 36, render.Regular, render.Left, render.Black)
	}
	y += 120
	c.Text(40, y, "Software update looks for a newer release, installs", 36, render.Regular, render.Left, render.Dark)
	c.Text(40, y+44, "it and restarts the app.", 36, render.Regular, render.Left, render.Dark)
	if v.UpdateMsg != "" {
		c.Text(40, y+104, fitText(c, v.UpdateMsg, 42, render.Bold, b.Dx()-80), 42, render.Bold, render.Left, render.Black)
	}
}

// IdleScreen is what the dashboard shows while its idle switch is off: IDLE, big, and
// what wakes it. An e-ink screen keeps its picture with no power, so it costs nothing
// to leave up.
func IdleScreen(c *render.Canvas, path string) {
	b := c.Bounds()
	const probe = 200.0
	w := float64(c.TextWidth("IDLE", probe, render.Bold))
	size := probe * float64(b.Dx()) * 0.72 / w
	mid := b.Dy() * 44 / 100
	c.Text(b.Dx()/2, mid+int(size*0.35), "IDLE", size, render.Bold, render.Center, render.Black)
	y := mid + int(size*0.35) + 110
	c.Text(b.Dx()/2, y, "The switch is off:", 44, render.Regular, render.Center, render.Black)
	c.Text(b.Dx()/2, y+60, fitText(c, path, 40, render.Bold, b.Dx()-80), 40, render.Bold, render.Center, render.Black)
	c.Text(b.Dx()/2, y+130, "Switch it on, or tap the screen, to resume", 44, render.Regular, render.Center, render.Black)
}

// drawNoGPS draws the GPS timeout picker.
func drawNoGPS(c *render.Canvas, v SettingsView) {
	b := c.Bounds()
	for i, ch := range NoGPSChoices {
		sec := ch.Seconds
		row(c, i, ch.Label, func(cy int) { radio(c, b.Dx()-80, cy, v.NoGPS == sec) })
	}
	for i, l := range []string{
		"The header shows NO DATA when our own GPS position has",
		"not updated for this long. A GPS fix comes about once a",
		"second, so 10 seconds is about ten missed fixes.",
	} {
		c.Text(40, settingsTop+len(NoGPSChoices)*settingsRowH+60+i*46, l, 36, render.Regular, render.Left, render.Dark)
	}
}
