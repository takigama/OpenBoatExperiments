package main

import (
	"os"
	"path/filepath"
	"testing"

	"signalkpaperdisplay/internal/pages"
)

func TestPowerCommands(t *testing.T) {
	dir := "/mnt/us/signalk"
	for kind, want := range map[string][]string{
		pages.PowerRestart: {"reboot"},
		pages.PowerOff:     {"poweroff"},
		pages.PowerStock:   {"sh", dir + "/startpaper.sh"},
	} {
		_, argv, err := powerCommand(kind, dir)
		if err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		if len(argv) != len(want) {
			t.Fatalf("%s: argv = %v, want %v", kind, argv, want)
		}
		for i := range want {
			if argv[i] != want[i] {
				t.Errorf("%s: argv = %v, want %v", kind, argv, want)
			}
		}
	}
	if _, _, err := powerCommand("format-everything", dir); err == nil {
		t.Error("an unknown choice must be refused, not run")
	}
	// Every choice the screen offers is one the command side knows.
	for _, c := range pages.PowerChoices {
		if _, argv, err := powerCommand(c.ID, dir); err != nil || len(argv) == 0 {
			t.Errorf("the screen offers %q but there is no command for it: %v", c.ID, err)
		}
	}
}

func TestStockChoiceWritesTheDisableFileFirst(t *testing.T) {
	dir := t.TempDir()
	before, _, err := powerCommand(pages.PowerStock, dir)
	if err != nil || before == nil {
		t.Fatalf("stock should have a step before the command: %v", err)
	}
	if err := before(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "disable")); err != nil {
		t.Errorf("the launcher's disable file was not written: %v", err)
	}
	if b, _, _ := powerCommand(pages.PowerOff, dir); b != nil {
		t.Error("powering off needs nothing written first")
	}
}

func TestRunPowerReportsAFailureToStart(t *testing.T) {
	// Run the real thing for the stock choice against a directory with no
	// launcher in it: sh starts, finds no script, and exits - harmless - and the
	// disable file is written. For a command that cannot start at all, the error
	// comes back.
	dir := t.TempDir()
	if err := runPower(pages.PowerStock, dir); err != nil {
		t.Fatalf("runPower(stock): %v", err)
	}
	if err := runPower("nonsense", dir); err == nil {
		t.Error("an unknown choice should fail")
	}
}
