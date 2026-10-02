package main

import (
	"log"

	"signalkpaperdisplay/internal/frontlight"
	"signalkpaperdisplay/internal/profile"
)

// detectLight finds the device's front light from its profile. Without one the
// backlight setting is simply not offered. For PNG previews on a PC there is
// no device to find, so a stand-in light lets the settings screens be drawn.
func detectLight(prof *profile.Profile, displayKind, settingsView string) frontlight.Light {
	if displayKind == "png" {
		if settingsView != "" {
			return frontlight.NewFake(24, 12)
		}
		return nil
	}
	if prof.Frontlight == nil {
		return nil
	}
	l, err := frontlight.Detect(frontlight.Config{
		Sysfs: prof.Frontlight.Sysfs, Steps: prof.Frontlight.Steps, Gamma: prof.Frontlight.Gamma, Lipc: prof.Frontlight.Lipc, LipcMax: prof.Frontlight.LipcMax,
	})
	if err != nil {
		log.Printf("front light: %v - the backlight setting is not available", err)
		return nil
	}
	log.Printf("front light: %d levels", l.Max())
	return l
}
