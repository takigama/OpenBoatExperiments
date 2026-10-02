// Package profile describes one target device (screen size, touch input).
// Profiles are plain JSON under platforms/<name>/profile.json so that
// supporting a new device never needs a code change - the same binary logic
// runs everywhere and just reads a different profile.
package profile

import (
	"encoding/json"
	"fmt"
	"os"
)

type Touch struct {
	Device   string `json:"device"`   // evdev node, e.g. /dev/input/event1
	Protocol string `json:"protocol"` // "mt-slot" (type-B multitouch), others later

	// How the panel's axes relate to the screen. Not detectable, so they're
	// measured once per device with `paperdisplay -touch-test` and recorded.
	SwapXY  bool `json:"swapXY,omitempty"`
	InvertX bool `json:"invertX,omitempty"`
	InvertY bool `json:"invertY,omitempty"`
}

type Profile struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	Width       int    `json:"width"`
	Height      int    `json:"height"`
	GrayLevels  int    `json:"grayLevels"` // distinct shades the panel can show
	Rotation    int    `json:"rotation"`   // degrees clockwise to apply before display
	Touch       *Touch `json:"touch,omitempty"`
	// Fetch is how this device downloads updates: "curl" where the device's
	// own curl is known to reach GitHub, otherwise Go's built-in HTTP.
	Fetch string `json:"fetch,omitempty"`
	// Frontlight says how to reach the device's built-in light; absent means
	// the device has none (or it is not supported), and the setting is hidden.
	Frontlight *Frontlight `json:"frontlight,omitempty"`
}

// Frontlight locates a device's light; see package frontlight.
type Frontlight struct {
	Sysfs   string  `json:"sysfs,omitempty"`   // glob for the backlight directory
	Steps   int     `json:"steps,omitempty"`   // levels to offer on a sysfs light with a much finer raw range
	Gamma   float64 `json:"gamma,omitempty"`   // curve from step to raw value; 0 means 2
	Lipc    string  `json:"lipc,omitempty"`    // "service property", used if there is no sysfs light
	LipcMax int     `json:"lipcMax,omitempty"` // how many levels the lipc property has
}

func Load(path string) (*Profile, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var p Profile
	if err := json.Unmarshal(b, &p); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if p.Width <= 0 || p.Height <= 0 {
		return nil, fmt.Errorf("%s: width and height must be set", path)
	}
	return &p, nil
}
