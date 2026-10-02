package main

import (
	"io"
	"os"
)

// capWriter is the log's destination: a file (normally the one the launcher
// redirects our output to) that is emptied when it grows past max bytes, so a
// log can never fill the Kindle's small user partition however long the app
// runs. A marker line says it happened. Anything that is not a regular file -
// a terminal, a pipe - is passed through untouched.
type capWriter struct {
	f   *os.File
	max int64
}

func (w *capWriter) Write(p []byte) (int, error) {
	if st, err := w.f.Stat(); err == nil && st.Mode().IsRegular() && st.Size()+int64(len(p)) > w.max {
		if w.f.Truncate(0) == nil {
			// Seek so a writer that was not opened for appending doesn't
			// leave a hole of the old length ahead of the next line.
			w.f.Seek(0, io.SeekStart)
			w.f.WriteString("log reached its size limit and was cleared\n")
		}
	}
	return w.f.Write(p)
}
