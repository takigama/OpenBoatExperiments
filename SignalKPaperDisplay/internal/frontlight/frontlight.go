// Package frontlight sets the brightness of a device's built-in light. How
// that is done differs by device, so a Light hides it: the Kindle's light is a
// Linux backlight (a "brightness" file under /sys/class/backlight), or
// failing that a property of its power daemon, reached through lipc.
package frontlight

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Light is a front light with levels 0 (off) up to Max.
type Light interface {
	Max() int
	Level() (int, error)
	Set(level int) error
}

// Config says where a device's light is. Either part may be empty.
type Config struct {
	// Sysfs is a glob for the backlight directory, e.g.
	// "/sys/class/backlight/*"; the directory must hold "brightness" and
	// "max_brightness" files.
	Sysfs string
	// Lipc is the "service property" pair to use when there is no sysfs
	// backlight, e.g. "com.lab126.powerd flIntensity", with LipcMax levels.
	Lipc    string
	LipcMax int
}

// Detect returns the light described by cfg, preferring sysfs (a plain file
// write: instant, with no helper process), or an error if there isn't one.
func Detect(cfg Config) (Light, error) {
	if cfg.Sysfs != "" {
		if l, err := NewSysfs(cfg.Sysfs); err == nil {
			return l, nil
		}
	}
	if cfg.Lipc != "" && cfg.LipcMax > 0 {
		if _, err := exec.LookPath("lipc-set-prop"); err == nil {
			return &Lipc{Prop: cfg.Lipc, MaxLevel: cfg.LipcMax}, nil
		}
	}
	return nil, fmt.Errorf("no front light found")
}

func clamp(level, max int) int { return max0(min(level, max)) }
func max0(v int) int {
	if v < 0 {
		return 0
	}
	return v
}

// Sysfs is a Linux backlight device.
type Sysfs struct {
	dir string
	max int
}

// NewSysfs finds the first directory matching glob that has a usable
// brightness file and a maximum.
func NewSysfs(glob string) (*Sysfs, error) {
	dirs, _ := filepath.Glob(glob)
	for _, d := range dirs {
		b, err := os.ReadFile(filepath.Join(d, "max_brightness"))
		if err != nil {
			continue
		}
		max, err := strconv.Atoi(strings.TrimSpace(string(b)))
		if err != nil || max <= 0 {
			continue
		}
		if _, err := os.Stat(filepath.Join(d, "brightness")); err != nil {
			continue
		}
		return &Sysfs{dir: d, max: max}, nil
	}
	return nil, fmt.Errorf("no backlight matches %q", glob)
}

func (s *Sysfs) Max() int { return s.max }

func (s *Sysfs) Level() (int, error) {
	b, err := os.ReadFile(filepath.Join(s.dir, "brightness"))
	if err != nil {
		return 0, err
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil {
		return 0, err
	}
	return clamp(n, s.max), nil
}

func (s *Sysfs) Set(level int) error {
	f, err := os.OpenFile(filepath.Join(s.dir, "brightness"), os.O_WRONLY|os.O_TRUNC, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = f.WriteString(strconv.Itoa(clamp(level, s.max)))
	return err
}

// Lipc is a light controlled through a lipc property.
type Lipc struct {
	Prop     string // "service property"
	MaxLevel int

	// run executes a command and returns its output; nil means really run it.
	// Tests replace it.
	run func(ctx context.Context, name string, args ...string) (string, error)
}

func (l *Lipc) Max() int { return l.MaxLevel }

func (l *Lipc) exec(name string, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if l.run != nil {
		return l.run(ctx, name, args...)
	}
	out, err := exec.CommandContext(ctx, name, args...).CombinedOutput()
	return string(out), err
}

func (l *Lipc) Level() (int, error) {
	out, err := l.exec("lipc-get-prop", strings.Fields(l.Prop)...)
	if err != nil {
		return 0, fmt.Errorf("lipc-get-prop: %w", err)
	}
	n, err := strconv.Atoi(strings.TrimSpace(out))
	if err != nil {
		return 0, fmt.Errorf("lipc-get-prop gave %q", strings.TrimSpace(out))
	}
	return clamp(n, l.MaxLevel), nil
}

func (l *Lipc) Set(level int) error {
	args := append([]string{"-i"}, strings.Fields(l.Prop)...)
	args = append(args, strconv.Itoa(clamp(level, l.MaxLevel)))
	if out, err := l.exec("lipc-set-prop", args...); err != nil {
		return fmt.Errorf("lipc-set-prop: %w (%s)", err, strings.TrimSpace(out))
	}
	return nil
}
