// Package display is the output side: something that can show a finished
// grayscale frame. The PNG implementation is for previews on a PC; device
// implementations (FBInk on Kindle/Kobo) sit alongside it and are selected
// at runtime, so pages and layout never know what they're drawing to.
package display

import "image"

// RegionDisplay is implemented by displays that can redraw a small
// rectangle on its own, much faster than a whole frame - used for things
// like the once-a-second heartbeat dot.
type RegionDisplay interface {
	// ShowRegion draws img with its top-left corner at (x, y) on screen.
	ShowRegion(img *image.Gray, x, y int) error
}

type Display interface {
	// Size is the frame size Show expects, in pixels.
	Size() (w, h int)
	// Show puts img on screen. fullRefresh asks e-ink panels for a flashing
	// full-screen refresh (clears ghosting); other displays ignore it. drew
	// is false when the frame was identical to what's already shown and
	// nothing was sent to the panel.
	Show(img *image.Gray, fullRefresh bool) (drew bool, err error)
}
