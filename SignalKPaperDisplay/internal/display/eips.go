package display

import (
	"context"
	"fmt"
	"hash/fnv"
	"image"
	"os/exec"
	"time"
)

// Eips draws via the Kindle's built-in `eips` tool: each frame is written
// to a PNG and handed to `eips -g`. No extra binaries to install, which is
// why it's the first device driver; an FBInk driver (also covering Kobo)
// would sit beside it behind the same interface.
type Eips struct {
	W, H int
	Bin  string // path to eips, normally /usr/sbin/eips
	Tmp  string // where the frame PNG is staged; point it at tmpfs (/var/tmp)

	png      PNG
	last     uint64
	haveLast bool
	runEips  func(ctx context.Context, args ...string) ([]byte, error)
}

func (e *Eips) Size() (int, int) { return e.W, e.H }

func frameHash(img *image.Gray) uint64 {
	h := fnv.New64a()
	h.Write(img.Pix)
	return h.Sum64()
}

func (e *Eips) Show(img *image.Gray, fullRefresh bool) error {
	// An unchanged frame isn't worth a panel refresh - every redraw costs
	// ghosting and a flash of work for nothing. A forced full refresh still
	// goes through, since its whole point is to clear the panel.
	sum := frameHash(img)
	if e.haveLast && sum == e.last && !fullRefresh {
		return nil
	}

	e.png = PNG{W: e.W, H: e.H, Path: e.Tmp}
	if err := e.png.Show(img, false); err != nil {
		return err
	}

	args := []string{"-g", e.Tmp}
	if fullRefresh {
		args = []string{"-f", "-g", e.Tmp}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	run := e.runEips
	if run == nil {
		run = func(ctx context.Context, args ...string) ([]byte, error) {
			return exec.CommandContext(ctx, e.Bin, args...).CombinedOutput()
		}
	}
	if out, err := run(ctx, args...); err != nil {
		return fmt.Errorf("eips %v: %w: %s", args, err, out)
	}
	e.last, e.haveLast = sum, true
	return nil
}
