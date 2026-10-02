package display

import (
	"image"
	"image/png"
	"os"
	"path/filepath"
)

// PNG writes every frame to a file, replacing it atomically so a viewer
// reloading the file never sees a half-written image.
type PNG struct {
	W, H int
	Path string
	// Fast skips compression. Frames staged for a drawing tool are written
	// and read back within a second, so compressing them only costs CPU -
	// maximum compression of a 1.5MB frame took seconds on a Kindle.
	Fast bool
}

func (p *PNG) Size() (int, int) { return p.W, p.H }

func (p *PNG) Show(img *image.Gray, _ bool) (bool, error) {
	dir := filepath.Dir(p.Path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return false, err
	}
	tmp, err := os.CreateTemp(dir, ".frame-*.png")
	if err != nil {
		return false, err
	}
	enc := png.Encoder{CompressionLevel: png.BestCompression}
	if p.Fast {
		enc.CompressionLevel = png.NoCompression
	}
	if err := enc.Encode(tmp, img); err != nil {
		tmp.Close()
		os.Remove(tmp.Name())
		return false, err
	}
	if err := tmp.Close(); err != nil {
		os.Remove(tmp.Name())
		return false, err
	}
	return true, os.Rename(tmp.Name(), p.Path)
}
