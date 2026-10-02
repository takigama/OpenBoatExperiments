package main

import (
	"log"
	"strconv"
	"strings"

	"signalkpaperdisplay/internal/battery"
)

// detectBattery finds the device's battery. On a PC (PNG previews) there is
// no device battery, so -fake-battery supplies one; without it nothing shows.
func detectBattery(displayKind, fake string) battery.Reader {
	if displayKind == "png" {
		if fake == "" {
			return nil
		}
		plugged := strings.HasSuffix(fake, "+")
		pct, err := strconv.Atoi(strings.TrimSuffix(fake, "+"))
		if err != nil {
			log.Fatalf("-fake-battery wants a number like 87 or 62+, got %q", fake)
		}
		return &battery.Fake{S: battery.Status{Percent: pct, Plugged: plugged, Charging: plugged && pct < 100}}
	}
	r, err := battery.Detect()
	if err != nil {
		log.Printf("battery: %v - the header will not show one", err)
		return nil
	}
	return r
}
