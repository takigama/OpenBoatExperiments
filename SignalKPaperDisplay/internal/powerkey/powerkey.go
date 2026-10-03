// Package powerkey reports presses of the Kindle's power button.
//
// The button is wired to the power chip, not to an input device, so there is no
// event file to read. The kernel announces each press as a uevent on its netlink
// socket instead ("Power button pressed, send user event KOBJ_ONLINE" in its
// log), and that is what is listened for here. powerd hears the press too, but
// ignores it for as long as the screensaver is prevented, which the launcher
// does so the dashboard never goes to sleep: so without this the button does
// nothing at all.
package powerkey

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"syscall"
	"time"
)

// Event is one kernel uevent.
type Event struct {
	Action string            // "online", "change", ...
	Path   string            // the device's path under /sys
	Env    map[string]string // the KEY=value lines that follow
}

// Parse reads one kernel uevent message: "action@devpath", then NUL-separated
// KEY=value pairs. Messages from udev start with a different header ("libudev")
// and are not parsed.
func Parse(msg []byte) (Event, bool) {
	parts := bytes.Split(bytes.TrimRight(msg, "\x00"), []byte{0})
	head := string(parts[0])
	action, path, ok := strings.Cut(head, "@")
	if !ok || action == "" || strings.HasPrefix(head, "libudev") {
		return Event{}, false
	}
	ev := Event{Action: action, Path: path, Env: map[string]string{}}
	for _, p := range parts[1:] {
		if k, v, ok := strings.Cut(string(p), "="); ok {
			ev.Env[k] = v
		}
	}
	return ev, true
}

// String is the event on one line, for logging.
func (e Event) String() string {
	keys := make([]string, 0, len(e.Env))
	for k, v := range e.Env {
		keys = append(keys, k+"="+v)
	}
	return fmt.Sprintf("%s@%s %s", e.Action, e.Path, strings.Join(keys, " "))
}

// powerButtonDrivers are the drivers of the power chips whose power button
// announces itself as an "online" uevent on the chip's own i2c device. Seen on
// the Kindle 8th generation (bd7181x); other Kindles' chips are added as they are
// checked with `paperdisplay -key-test`.
var powerButtonDrivers = map[string]bool{"bd7181x": true}

// IsPowerButton reports whether a uevent is a press of the power button: the
// kernel's "send user event KOBJ_ONLINE" from the power chip's driver. (The
// same chip also reports battery and charger changes, as "change" events on its
// power_supply devices; those are not presses.)
func IsPowerButton(e Event) bool {
	return e.Action == "online" && powerButtonDrivers[e.Env["DRIVER"]]
}

// Listen reports every uevent the kernel sends until ctx is cancelled.
func Listen(ctx context.Context, handle func(Event)) error {
	fd, err := syscall.Socket(syscall.AF_NETLINK, syscall.SOCK_DGRAM, 15 /* NETLINK_KOBJECT_UEVENT */)
	if err != nil {
		return err
	}
	if err := syscall.Bind(fd, &syscall.SockaddrNetlink{Family: syscall.AF_NETLINK, Pid: uint32(os.Getpid()), Groups: 1}); err != nil {
		syscall.Close(fd)
		return err
	}
	// A read timeout lets the loop notice the context ending: closing a file
	// descriptor does not reliably wake a read already blocked on it.
	tv := syscall.NsecToTimeval(int64(time.Second))
	syscall.SetsockoptTimeval(fd, syscall.SOL_SOCKET, syscall.SO_RCVTIMEO, &tv)
	defer syscall.Close(fd)

	buf := make([]byte, 16384)
	for ctx.Err() == nil {
		n, _, err := syscall.Recvfrom(fd, buf, 0)
		if err != nil {
			if errors.Is(err, syscall.EAGAIN) || errors.Is(err, syscall.EINTR) {
				continue
			}
			return err
		}
		if ev, ok := Parse(buf[:n]); ok {
			handle(ev)
		}
	}
	return nil
}
