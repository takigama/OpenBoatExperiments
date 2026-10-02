package display

import (
	"context"
	"errors"
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
	// RegionArgs builds the arguments for drawing a small PNG at (x, y) and
	// refreshing only that rectangle; nil if the tool can't (see ShowRegion).
	RegionArgs func(png string, x, y int) []string
	// IgnoreRect is excluded when deciding whether a frame changed. Whatever
	// lives there (the heartbeat dot) is updated separately through
	// ShowRegion, so it must not make every frame look new.
	IgnoreRect image.Rectangle

	png      PNG
	last     uint64
	haveLast bool
	run      func(ctx context.Context, bin string, args ...string) ([]byte, error)

	encodeTook, toolTook time.Duration // for the last draw, see LastTiming
}

// LastTiming describes where the last draw's time went, so slow refreshes
// can be traced to our own encoding or to the drawing tool itself.
func (e *Exec) LastTiming() string {
	return fmt.Sprintf("encode %s, %s %s", e.encodeTook.Round(time.Millisecond),
		filepath.Base(e.Bin), e.toolTook.Round(time.Millisecond))
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
		},
		RegionArgs: func(png string, x, y int) []string {
			// A region is always the fast waveform: it's a few dozen pixels
			// and is redrawn constantly.
			wf := waveform
			if wf == "" {
				wf = "DU"
			}
			return []string{"-q", "-w", "-g", fmt.Sprintf("file=%s,x=%d,y=%d", png, x, y), "-W", wf}
		}}
}

func (e *Exec) Size() (int, int) { return e.W, e.H }

// ErrNoRegion is returned by ShowRegion for tools that can't draw a region.
var ErrNoRegion = errors.New("display cannot update a region on its own")

// ShowRegion draws img with its top-left at (x, y) and refreshes only that
// rectangle.
func (e *Exec) ShowRegion(img *image.Gray, x, y int) error {
	if e.RegionArgs == nil {
		return ErrNoRegion
	}
	path := e.Tmp + ".region.png"
	region := PNG{W: img.Bounds().Dx(), H: img.Bounds().Dy(), Path: path, Fast: true}
	if _, err := region.Show(img, false); err != nil {
		return err
	}
	args := e.RegionArgs(path, x, y)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if out, err := e.runner()(ctx, e.Bin, args...); err != nil {
		return fmt.Errorf("%s %v: %w: %s", filepath.Base(e.Bin), args, err, out)
	}
	return nil
}

func (e *Exec) runner() func(ctx context.Context, bin string, args ...string) ([]byte, error) {
	if e.run != nil {
		return e.run
	}
	return func(ctx context.Context, bin string, args ...string) ([]byte, error) {
		return exec.CommandContext(ctx, bin, args...).CombinedOutput()
	}
}

// frameHash fingerprints img, leaving out ignore: pixels there don't count
// towards "has the picture changed".
func frameHash(img *image.Gray, ignore image.Rectangle) uint64 {
	h := fnv.New64a()
	b := img.Bounds()
	ignore = ignore.Intersect(b)
	for y := b.Min.Y; y < b.Max.Y; y++ {
		row := img.Pix[(y-b.Min.Y)*img.Stride : (y-b.Min.Y)*img.Stride+b.Dx()]
		if ignore.Empty() || y < ignore.Min.Y || y >= ignore.Max.Y {
			h.Write(row)
			continue
		}
		h.Write(row[:ignore.Min.X-b.Min.X])
		h.Write(row[ignore.Max.X-b.Min.X:])
	}
	return h.Sum64()
}

func (e *Exec) Show(img *image.Gray, fullRefresh bool) (bool, error) {
	// An unchanged frame isn't worth a panel refresh - every redraw costs
	// ghosting and a flash of work for nothing. A forced full refresh still
	// goes through, since its whole point is to clear the panel.
	sum := frameHash(img, e.IgnoreRect)
	if e.haveLast && sum == e.last && !fullRefresh {
		return false, nil
	}

	t0 := time.Now()
	e.png = PNG{W: e.W, H: e.H, Path: e.Tmp, Fast: true}
	if _, err := e.png.Show(img, false); err != nil {
		return false, err
	}
	e.encodeTook = time.Since(t0)

	args := e.Args(e.Tmp, fullRefresh)
	t1 := time.Now()
	defer func() { e.toolTook = time.Since(t1) }()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if out, err := e.runner()(ctx, e.Bin, args...); err != nil {
		return false, fmt.Errorf("%s %v: %w: %s", filepath.Base(e.Bin), args, err, out)
	}
	e.last, e.haveLast = sum, true
	return true, nil
}
