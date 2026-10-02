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
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"signalkpaperdisplay/internal/app"
	"signalkpaperdisplay/internal/display"
	"signalkpaperdisplay/internal/pages"
	"signalkpaperdisplay/internal/profile"
	"signalkpaperdisplay/internal/signalk"
	"signalkpaperdisplay/internal/units"
)

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

const defaultManifest = "https://raw.githubusercontent.com/takigama/OpenBoatExperiments/master/SignalKPaperDisplay/update/manifest.json"

// forceExitAfterCancel guarantees the process dies shortly after Ctrl-C or
// SIGTERM. Closing a device file does not interrupt a read already blocked
// on it, and an eips child can hang, so without this a stuck goroutine can
// keep the process alive - which on a boat means a hard kill.
func forceExitAfterCancel(ctx context.Context) {
	go func() {
		<-ctx.Done()
		time.Sleep(2 * time.Second)
		log.Print("shutdown timed out, forcing exit")
		os.Exit(1)
	}()
}

func main() {
	var (
		server       = flag.String("signalk", "localhost:3000", "SignalK server host:port")
		token        = flag.String("token", "", "SignalK bearer token, if the server needs one")
		profilePath  = flag.String("profile", "", "path to a platforms/<name>/profile.json (required)")
		displayKind  = flag.String("display", "png", "output: png (preview file), eips (Kindle, slow) or fbink (Kindle/Kobo)")
		out          = flag.String("out", "out/frame.png", "output file for -display png")
		eipsBin      = flag.String("eips", "/usr/sbin/eips", "eips binary, for -display eips")
		fbinkBin     = flag.String("fbink", "", "fbink binary, for -display fbink (default: the one beside this binary; KOReader's bundled one can't draw images)")
		waveform     = flag.String("waveform", "DU", "e-ink waveform for partial updates with -display fbink: DU is fast (~290ms on a Paperwhite 3, coarse grays), GL16 or \"\" (FBInk's choice) is slower (~540ms) but smoother; full refreshes always use full quality")
		eipsTmp      = flag.String("tmp", "/var/tmp/paperdisplay.png", "staging PNG for eips/fbink (use tmpfs, not flash)")
		pageID       = flag.String("page", "compass", "page to start on: compass, nav")
		settingsView = flag.String("settings-view", "", "start on a settings screen (for previews): root, preset or unit:<metric>")
		settingsPath = flag.String("settings", "settings.json", "unit settings file (missing = metric defaults)")
		fullEvery    = flag.Duration("full-refresh", 5*time.Minute, "flashing full refresh interval, to clear e-ink ghosting")
		once         = flag.Bool("once", false, "render a single frame after -wait, then exit (for previews)")
		wait         = flag.Duration("wait", 3*time.Second, "with -once: how long to collect data first")
		interval     = flag.Duration("interval", time.Second, "redraw interval")
		touch        = flag.Bool("touch", false, "read the touchscreen named in the profile (tap sides / swipe to change page)")
		minRefresh   = flag.Duration("min-refresh", 2*time.Second, "shortest gap between partial refreshes (0 = redraw on every change); page changes ignore it")
		manifestURL  = flag.String("manifest", defaultManifest, "update manifest URL")
		fetchKind    = flag.String("fetch", "", "how to download updates: curl or http (default: the profile's setting, else http)")
		curlBin      = flag.String("curl", "curl", "curl binary, for -fetch curl")
		checkUpdate  = flag.Bool("check-update", false, "report whether a newer release exists, then exit")
		doUpdate     = flag.Bool("update", false, "install a newer release if there is one, then exit")
		updateEvery  = flag.Duration("update-every", 0, "check for and install updates this often while running (0 = never); the launcher restarts the new version")
		showVersion  = flag.Bool("version", false, "print the version and exit")
		touchTest    = flag.Bool("touch-test", false, "print raw and mapped touch events, then exit on Ctrl-C (to measure a device's orientation)")
	)
	flag.Parse()

	// On a device the profile is deployed right beside the binary, so don't
	// make anyone spell out its path.
	if *profilePath == "" {
		if exe, err := os.Executable(); err == nil {
			if cand := filepath.Join(filepath.Dir(exe), "profile.json"); fileExists(cand) {
				*profilePath = cand
			}
		}
	}
	if *profilePath == "" {
		log.Fatal("-profile is required (or put profile.json next to the binary), e.g. -profile platforms/kindle-pw3/profile.json")
	}
	prof, err := profile.Load(*profilePath)
	if err != nil {
		log.Fatal(err)
	}

	if *showVersion {
		log.Printf("paperdisplay v%s, platform %s", version, prof.Name)
		return
	}

	kind := *fetchKind
	if kind == "" {
		kind = prof.Fetch
	}
	fetcher := newFetcher(kind, *curlBin)
	if *checkUpdate || *doUpdate {
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		forceExitAfterCancel(ctx)
		if _, err := runUpdate(ctx, fetcher, *manifestURL, prof.Name, *doUpdate); err != nil {
			log.Fatalf("update: %v", err)
		}
		return
	}

	if *touchTest {
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		forceExitAfterCancel(ctx)
		runTouchTest(ctx, prof)
		return
	}

	var disp display.Display
	switch *displayKind {
	case "png":
		disp = &display.PNG{W: prof.Width, H: prof.Height, Path: *out}
	case "eips":
		disp = display.NewEips(prof.Width, prof.Height, *eipsBin, *eipsTmp)
	case "fbink":
		bin := *fbinkBin
		if bin == "" {
			exe, err := os.Executable()
			if err != nil {
				log.Fatal(err)
			}
			bin = filepath.Join(filepath.Dir(exe), "fbink")
		}
		disp = display.NewFBInk(prof.Width, prof.Height, bin, *eipsTmp, *waveform)
	default:
		log.Fatalf("unknown -display %q", *displayKind)
	}
	// The heartbeat dot is updated on its own, so a frame that differs only
	// there is not a changed frame.
	if ex, ok := disp.(*display.Exec); ok {
		ex.IgnoreRect = pages.HeartbeatRect(prof.Width)
	}

	state := signalk.NewState()
	client := &signalk.Client{URL: signalk.StreamURL(*server), Token: *token, State: state}
	unitSettings, err := units.Load(*settingsPath)
	if err != nil {
		log.Fatal(err)
	}
	a := &app.App{State: state, Display: disp, Interval: *interval, FullRefreshEvery: *fullEvery, MinRefresh: *minRefresh, Units: unitSettings, UnitsPath: *settingsPath}

	if !a.SetPage(*pageID) {
		log.Fatalf("unknown -page %q", *pageID)
	}
	switch v := *settingsView; {
	case v == "":
	case v == "root":
		a.OpenSettings(pages.SettingsView{})
	case v == "preset":
		a.OpenSettings(pages.SettingsView{Screen: pages.SettingsPickPreset})
	case strings.HasPrefix(v, "unit:"):
		a.OpenSettings(pages.SettingsView{Screen: pages.SettingsPickUnit, Metric: strings.TrimPrefix(v, "unit:")})
	default:
		log.Fatalf("unknown -settings-view %q (want root, preset or unit:<metric>)", v)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	forceExitAfterCancel(ctx)
	go client.Run(ctx)

	if *updateEvery > 0 && !*once {
		go autoUpdate(ctx, fetcher, *manifestURL, prof.Name, *updateEvery, stop)
	}

	if *touch {
		dev, err := openTouch(prof)
		if err != nil {
			log.Fatalf("touch: %v", err)
		}
		defer dev.Close()
		go func() {
			if err := dev.Run(ctx, nil, a.HandleEvent); err != nil {
				log.Printf("touch stopped: %v", err)
			}
		}()
	}

	if *once {
		time.Sleep(*wait)
		if _, err := a.Show(time.Now(), true); err != nil {
			log.Fatal(err)
		}
		log.Printf("wrote %s (%dx%d, profile %s)", *out, prof.Width, prof.Height, prof.Name)
		return
	}
	a.Run(ctx)
}
