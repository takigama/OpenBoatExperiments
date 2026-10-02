package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func openAppend(t *testing.T, path string) *os.File {
	t.Helper()
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o644)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { f.Close() })
	return f
}

func TestLogNeverGrowsPastItsLimit(t *testing.T) {
	path := filepath.Join(t.TempDir(), "log")
	w := &capWriter{f: openAppend(t, path), max: 1000}
	line := strings.Repeat("x", 99) + "\n"
	for i := 0; i < 500; i++ {
		if _, err := w.Write([]byte(line)); err != nil {
			t.Fatal(err)
		}
		if st, _ := os.Stat(path); st.Size() > 1000 {
			t.Fatalf("log is %d bytes after %d lines, limit is 1000", st.Size(), i+1)
		}
	}
	b, _ := os.ReadFile(path)
	if !strings.Contains(string(b), "was cleared") {
		t.Error("clearing the log should leave a marker saying so")
	}
	if !strings.HasSuffix(string(b), line) {
		t.Error("the newest line must always survive")
	}
}

func TestLogFileNotOpenedForAppendingHasNoHole(t *testing.T) {
	// `2>file` (as opposed to `2>>file`) leaves the write offset where it was;
	// after the truncate the file must not become sparse.
	path := filepath.Join(t.TempDir(), "log")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	w := &capWriter{f: f, max: 500}
	for i := 0; i < 200; i++ {
		w.Write([]byte(strings.Repeat("y", 49) + "\n"))
	}
	st, _ := os.Stat(path)
	if st.Size() > 500 {
		t.Errorf("size %d: the offset was not rewound after clearing", st.Size())
	}
}

func TestSmallLogsAreLeftAlone(t *testing.T) {
	path := filepath.Join(t.TempDir(), "log")
	w := &capWriter{f: openAppend(t, path), max: 1 << 20}
	for i := 0; i < 10; i++ {
		w.Write([]byte("hello\n"))
	}
	b, _ := os.ReadFile(path)
	if string(b) != strings.Repeat("hello\n", 10) {
		t.Errorf("log = %q", b)
	}
}

func TestNonFilesPassThrough(t *testing.T) {
	// /dev/null is a character device, not a regular file: no truncating.
	f, err := os.OpenFile(os.DevNull, os.O_WRONLY, 0)
	if err != nil {
		t.Skip("no /dev/null")
	}
	defer f.Close()
	w := &capWriter{f: f, max: 1}
	if n, err := w.Write([]byte("abc")); err != nil || n != 3 {
		t.Errorf("write = %d, %v", n, err)
	}
}
