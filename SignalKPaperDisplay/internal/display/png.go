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
