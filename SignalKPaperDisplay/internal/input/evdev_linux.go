//go:build linux

package input

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"syscall"
	"time"
	"unsafe"
)

// Axis is one absolute axis's reported range.
type Axis struct{ Min, Max int32 }

// Orientation describes how the touch panel's axes relate to the screen,
// since the two often differ (swapped or flipped) and it can't be detected.
type Orientation struct {
	SwapXY, InvertX, InvertY bool
}

type Device struct {
	f      *os.File
	X, Y   Axis
	Screen struct{ W, H int }
	Orient Orientation
}

// input_absinfo from linux/input.h.
type absInfo struct {
	Value, Min, Max, Fuzz, Flat, Resolution int32
}

// EVIOCGABS(abs) = _IOR('E', 0x40+abs, struct input_absinfo)
func eviocgabs(abs uint) uintptr {
	const (
		read    = 2
		typ     = 'E'
		sizeOfI = 24
	)
	return uintptr(read<<30 | sizeOfI<<16 | typ<<8 | (0x40 + abs))
}

func getAbs(f *os.File, abs uint) (Axis, error) {
	var a absInfo
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), eviocgabs(abs), uintptr(unsafe.Pointer(&a)))
	if errno != 0 {
		return Axis{}, errno
	}
	return Axis{a.Min, a.Max}, nil
}

// Open opens an evdev touch node and reads its axis ranges.
func Open(path string, screenW, screenH int, o Orientation) (*Device, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	d := &Device{f: f, Orient: o}
	d.Screen.W, d.Screen.H = screenW, screenH
	if d.X, err = getAbs(f, absMTPositionX); err != nil {
		f.Close()
		return nil, fmt.Errorf("%s: reading X range: %w", path, err)
	}
	if d.Y, err = getAbs(f, absMTPositionY); err != nil {
		f.Close()
		return nil, fmt.Errorf("%s: reading Y range: %w", path, err)
	}
	return d, nil
}

func (d *Device) Close() error { return d.f.Close() }

// Map turns raw device coordinates into screen pixels.
func (d *Device) Map(rawX, rawY int32) (int, int) {
	nx := float64(rawX-d.X.Min) / float64(d.X.Max-d.X.Min)
	ny := float64(rawY-d.Y.Min) / float64(d.Y.Max-d.Y.Min)
	if d.Orient.SwapXY {
		nx, ny = ny, nx
	}
	if d.Orient.InvertX {
		nx = 1 - nx
	}
	if d.Orient.InvertY {
		ny = 1 - ny
	}
	x, y := int(nx*float64(d.Screen.W)), int(ny*float64(d.Screen.H))
	return min(max(x, 0), d.Screen.W-1), min(max(y, 0), d.Screen.H-1)
}

// Run reads events until ctx is cancelled or the device fails, calling
// onRaw (if set) for every raw event - used by the touch-test mode - and
// onEvent for each recognised gesture.
func (d *Device) Run(ctx context.Context, onRaw func(Raw), onEvent func(Event)) error {
	go func() { <-ctx.Done(); d.f.Close() }() // unblocks the read below

	tr := NewTracker(d.Map)
	// struct input_event is { struct timeval; u16 type; u16 code; s32 value },
	// and timeval is 8 bytes on 32-bit ARM, 16 on 64-bit.
	tvSize := int(unsafe.Sizeof(syscall.Timeval{}))
	buf := make([]byte, tvSize+8)
	for {
		if _, err := io.ReadFull(d.f, buf); err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		r := Raw{
			Type:  binary.LittleEndian.Uint16(buf[tvSize:]),
			Code:  binary.LittleEndian.Uint16(buf[tvSize+2:]),
			Value: int32(binary.LittleEndian.Uint32(buf[tvSize+4:])),
			At:    time.Now(),
		}
		if onRaw != nil {
			onRaw(r)
		}
		if ev, ok := tr.Feed(r); ok {
			onEvent(ev)
		}
	}
}
