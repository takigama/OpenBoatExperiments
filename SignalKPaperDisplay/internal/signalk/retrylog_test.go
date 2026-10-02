package signalk

import (
	"errors"
	"testing"
)

func TestRetryLogSaysWhatChangedNotWhatRepeats(t *testing.T) {
	var r retryLog
	down := errors.New("dial tcp 10.0.0.76:3001: connection refused")
	if !r.should(down) {
		t.Fatal("the first failure must be logged")
	}
	logged := 0
	for i := 0; i < 600; i++ {
		if r.should(down) {
			logged++
		}
	}
	if logged != 10 {
		t.Errorf("600 repeats logged %d times, want 10 (one in sixty)", logged)
	}
	if !r.should(errors.New("websocket: bad handshake")) {
		t.Error("a different failure is news and must be logged")
	}
	if !r.should(down) {
		t.Error("going back to the first failure is a change too")
	}
}
