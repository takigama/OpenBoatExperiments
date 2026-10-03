package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"signalkpaperdisplay/internal/pages"
)

// powerCommand is what to run for a power choice, given the directory the app
// is installed in (where the launcher and its `disable` file live).
//
//   - stock: the launcher's own switch. With a `disable` file present it stops the
//     dashboard and starts the Kindle's own jobs again, so write the file and run
//     it once now rather than wait for cron.
//   - restart / poweroff: the Kindle's own busybox commands.
func powerCommand(kind, dir string) (before func() error, argv []string, err error) {
	switch kind {
	case pages.PowerStock:
		return func() error {
			return os.WriteFile(filepath.Join(dir, "disable"), nil, 0o644)
		}, []string{"sh", filepath.Join(dir, "startpaper.sh")}, nil
	case pages.PowerRestart:
		return nil, []string{"reboot"}, nil
	case pages.PowerOff:
		return nil, []string{"poweroff"}, nil
	}
	return nil, nil, fmt.Errorf("unknown power choice %q", kind)
}

// runPower carries out a power choice. It starts the command and returns: for
// the stock software the launcher then kills this very process, and a reboot or
// power off takes it with the whole system.
func runPower(kind, dir string) error {
	before, argv, err := powerCommand(kind, dir)
	if err != nil {
		return err
	}
	if before != nil {
		if err := before(); err != nil {
			return err
		}
	}
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Dir = dir
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("%s: %w", argv[0], err)
	}
	go cmd.Wait()
	return nil
}
