package pages

import (
	"testing"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/units"
)

// BenchmarkCompass is a full compass frame as the Kindle draws it, with ships
// around: the thing worth timing, since it runs on a slow CPU about every
// other second.
func BenchmarkCompass(b *testing.B) {
	s := navBoat(3)
	e := Env{Units: units.Settings{Preset: units.PresetMetric}}
	for i := 0; i < b.N; i++ {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			b.Fatal(err)
		}
		Compass(c, s, compassNow, e)
	}
}

func BenchmarkNav(b *testing.B) {
	s := navBoat(3)
	e := Env{Units: units.Settings{Preset: units.PresetMetric}}
	for i := 0; i < b.N; i++ {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			b.Fatal(err)
		}
		Nav(c, s, compassNow, e)
	}
}
