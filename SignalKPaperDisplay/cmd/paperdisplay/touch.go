package main

import (
	"context"
	"errors"
	"log"

	"signalkpaperdisplay/internal/input"
	"signalkpaperdisplay/internal/profile"
)

func openTouch(prof *profile.Profile) (*input.Device, error) {
	if prof.Touch == nil {
		return nil, errors.New("profile has no touch section")
	}
	t := prof.Touch
	d, err := input.Open(t.Device, prof.Width, prof.Height,
		input.Orientation{SwapXY: t.SwapXY, InvertX: t.InvertX, InvertY: t.InvertY})
	if err != nil {
		return nil, err
	}
	if d.Grabbed {
		log.Printf("touch: %s grabbed exclusively (the stock UI no longer sees taps)", t.Device)
	} else {
		log.Printf("touch: could not grab %s exclusively - the stock UI may react to taps too", t.Device)
	}
	return d, nil
}

// runTouchTest prints what the touchscreen reports, so a device's axis
// orientation can be worked out by tapping known spots: tap the top-left
// corner and check the mapped position comes out near (0,0), and so on. The
// result goes into the platform's profile.json (swapXY/invertX/invertY).
func runTouchTest(ctx context.Context, prof *profile.Profile) {
	d, err := openTouch(prof)
	if err != nil {
		log.Fatalf("touch: %v", err)
	}
	defer d.Close()
	log.Printf("%s: raw X %d..%d, raw Y %d..%d -> screen %dx%d (swapXY=%v invertX=%v invertY=%v)",
		prof.Touch.Device, d.X.Min, d.X.Max, d.Y.Min, d.Y.Max, prof.Width, prof.Height,
		d.Orient.SwapXY, d.Orient.InvertX, d.Orient.InvertY)
	log.Println("tap the four corners and the middle; Ctrl-C to stop")

	err = d.Run(ctx,
		func(r input.Raw) {
			switch {
			case r.Type == 3 && r.Code == 0x35:
				log.Printf("  raw X=%d", r.Value)
			case r.Type == 3 && r.Code == 0x36:
				log.Printf("  raw Y=%d", r.Value)
			}
		},
		func(ev input.Event) { log.Printf("%s at screen (%d,%d)", ev.Kind, ev.X, ev.Y) })
	if err != nil {
		log.Fatal(err)
	}
}
