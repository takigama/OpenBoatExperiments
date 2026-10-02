// Package display is the output side: something that can show a finished
// grayscale frame. The PNG implementation is for previews on a PC; device
// implementations (FBInk on Kindle/Kobo) sit alongside it and are selected
// at runtime, so pages and layout never know what they're drawing to.
package display

import "image"

type Display interface {
	// Size is the frame size Show expects, in pixels.
	Size() (w, h int)
	// Show puts img on screen. fullRefresh asks e-ink panels for a flashing
	// full-screen refresh (clears ghosting); other displays ignore it.
	Show(img *image.Gray, fullRefresh bool) error
}
