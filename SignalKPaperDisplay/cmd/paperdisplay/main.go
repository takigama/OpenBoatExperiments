// paperdisplay shows SignalK data on an e-ink display. One binary for every
// platform: the device is chosen by -profile (platforms/<name>/profile.json)
// and the output by -display.
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"time"

	"signalkpaperdisplay/internal/app"
	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/profile"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func main() {
	var (
		server       = flag.String("signalk", "localhost:3000", "SignalK server host:port")
		token        = flag.String("token", "", "SignalK bearer token, if the server needs one")
		profilePath  = flag.String("profile", "", "path to a platforms/<name>/profile.json (required)")
		displayKind  = flag.String("display", "png", "output: png (preview file) or eips (Kindle screen)")
		out          = flag.String("out", "out/frame.png", "output file for -display png")
		eipsBin      = flag.String("eips", "/usr/sbin/eips", "eips binary, for -display eips")
		eipsTmp      = flag.String("tmp", "/var/tmp/paperdisplay.png", "staging PNG for -display eips (use tmpfs, not flash)")
		pageID       = flag.String("page", "nav", "page to show: nav, compass")
		settingsPath = flag.String("settings", "settings.json", "unit settings file (missing = metric defaults)")
		fullEvery    = flag.Duration("full-refresh", 5*time.Minute, "flashing full refresh interval, to clear e-ink ghosting")
		once         = flag.Bool("once", false, "render a single frame after -wait, then exit (for previews)")
		wait         = flag.Duration("wait", 3*time.Second, "with -once: how long to collect data first")
		interval     = flag.Duration("interval", time.Second, "redraw interval")
	)
	flag.Parse()

	if *profilePath == "" {
		log.Fatal("-profile is required, e.g. -profile platforms/kindle-pw3/profile.json")
	}
	prof, err := profile.Load(*profilePath)
	if err != nil {
		log.Fatal(err)
	}

	var disp display.Display
	switch *displayKind {
	case "png":
		disp = &display.PNG{W: prof.Width, H: prof.Height, Path: *out}
	case "eips":
		disp = &display.Eips{W: prof.Width, H: prof.Height, Bin: *eipsBin, Tmp: *eipsTmp}
	default:
		log.Fatalf("unknown -display %q", *displayKind)
	}

	state := signalk.NewState()
	client := &signalk.Client{URL: signalk.StreamURL(*server), Token: *token, State: state}
	unitSettings, err := units.Load(*settingsPath)
	if err != nil {
		log.Fatal(err)
	}
	a := &app.App{State: state, Display: disp, Interval: *interval, FullRefreshEvery: *fullEvery, Units: unitSettings}

	if !a.SetPage(*pageID) {
		log.Fatalf("unknown -page %q", *pageID)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	go client.Run(ctx)

	if *once {
		time.Sleep(*wait)
		if err := a.Show(time.Now(), true); err != nil {
			log.Fatal(err)
		}
		log.Printf("wrote %s (%dx%d, profile %s)", *out, prof.Width, prof.Height, prof.Name)
		return
	}
	a.Run(ctx)
}
