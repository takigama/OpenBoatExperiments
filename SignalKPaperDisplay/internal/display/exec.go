package display

import (
	"context"
	"fmt"
	"hash/fnv"
	"image"
	"os/exec"
	"path/filepath"
	"time"
)

// Exec draws by writing each frame to a PNG and running an external tool on
// it - eips on stock Kindles, FBInk on Kindle and Kobo. The tools differ
// only in their command lines (see NewEips / NewFBInk), so everything else
// - skipping unchanged frames, staging the PNG - lives here once.
type Exec struct {
	W, H int
	Bin  string // the drawing tool
	Tmp  string // where the frame PNG is staged; point it at tmpfs (/var/tmp)
	// Args builds the tool's arguments for drawing the staged PNG.
	Args func(png string, fullRefresh bool) []string

	png      PNG
	last     uint64
	haveLast bool
	run      func(ctx context.Context, bin string, args ...string) ([]byte, error)
}

// NewEips uses the Kindle's built-in eips: no extra binaries, but measured
// at ~3.5s per refresh on a Paperwhite 3.
func NewEips(w, h int, bin, tmp string) *Exec {
	return &Exec{W: w, H: h, Bin: bin, Tmp: tmp,
		Args: func(png string, full bool) []string {
			if full {
				return []string{"-f", "-g", png}
			}
			return []string{"-g", png}
		}}
}

// NewFBInk uses FBInk (https://github.com/NiLuJe/FBInk), which supports
// Kindle and Kobo and is much faster than eips. waveform picks the e-ink
// waveform for partial updates (e.g. GL16, DU); empty lets FBInk choose.
func NewFBInk(w, h int, bin, tmp, waveform string) *Exec {
	return &Exec{W: w, H: h, Bin: bin, Tmp: tmp,
		Args: func(png string, full bool) []string {
			// -w waits for the panel to finish, so Show's duration is the
			// real refresh time and updates can't pile up behind each other.
			args := []string{"-q", "-w", "-g", "file=" + png}
			switch {
			case full:
				args = append(args, "-f") // flash the whole panel
			case waveform != "":
				args = append(args, "-W", waveform)
			}
			return args
		}}
}

func (e *Exec) Size() (int, int) { return e.W, e.H }

func frameHash(img *image.Gray) uint64 {
	h := fnv.New64a()
	h.Write(img.Pix)
	return h.Sum64()
}

func (e *Exec) Show(img *image.Gray, fullRefresh bool) (bool, error) {
	// An unchanged frame isn't worth a panel refresh - every redraw costs
	// ghosting and a flash of work for nothing. A forced full refresh still
	// goes through, since its whole point is to clear the panel.
	sum := frameHash(img)
	if e.haveLast && sum == e.last && !fullRefresh {
		return false, nil
	}

	e.png = PNG{W: e.W, H: e.H, Path: e.Tmp}
	if _, err := e.png.Show(img, false); err != nil {
		return false, err
	}

	args := e.Args(e.Tmp, fullRefresh)
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	run := e.run
	if run == nil {
		run = func(ctx context.Context, bin string, args ...string) ([]byte, error) {
			return exec.CommandContext(ctx, bin, args...).CombinedOutput()
		}
	}
	if out, err := run(ctx, e.Bin, args...); err != nil {
		return false, fmt.Errorf("%s %v: %w: %s", filepath.Base(e.Bin), args, err, out)
	}
	e.last, e.haveLast = sum, true
	return true, nil
}
