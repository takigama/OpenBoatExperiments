package main

import (
	"context"
	"log"

	"signalkpaperdisplay/internal/powerkey"
)

// runKeyTest prints every kernel uevent as it arrives, to find out what the
// power button looks like on a device: press it and see which line appears.
func runKeyTest(ctx context.Context) {
	log.Print("key-test: press the power button; every kernel uevent is printed (Ctrl-C to stop)")
	if err := powerkey.Listen(ctx, func(e powerkey.Event) { log.Printf("uevent: %s", e) }); err != nil {
		log.Fatalf("key-test: %v", err)
	}
}
