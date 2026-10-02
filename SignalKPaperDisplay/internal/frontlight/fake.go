package frontlight

// Fake is an in-memory light, for previews on a PC and for tests.
type Fake struct {
	MaxLevel int
	Current  int
	Sets     []int // every level Set was called with, in order
}

func NewFake(max, level int) *Fake { return &Fake{MaxLevel: max, Current: level} }

func (f *Fake) Max() int            { return f.MaxLevel }
func (f *Fake) Level() (int, error) { return f.Current, nil }
func (f *Fake) Set(level int) error {
	f.Sets = append(f.Sets, level)
	f.Current = clamp(level, f.MaxLevel)
	return nil
}
