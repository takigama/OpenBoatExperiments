package powerkey

import "testing"

// Real messages from a Kindle 8th generation: the power button's press, and a
// battery report from the same chip.
const (
	pressMsg   = "online@/devices/soc0/soc.2/2100000.aips-bus/21a0000.i2c/i2c-0/0-004b\x00DEVPATH=/devices/soc0/soc.2/2100000.aips-bus/21a0000.i2c/i2c-0/0-004b\x00DRIVER=bd7181x\x00OF_COMPATIBLE_0=rohm,bd71815\x00OF_COMPATIBLE_N=1\x00SEQNUM=6082\x00ACTION=online\x00OF_NAME=bd7181x\x00SUBSYSTEM=i2c\x00"
	batteryMsg = "change@/devices/soc0/soc.2/2100000.aips-bus/21a0000.i2c/i2c-0/0-004b/bd7181x-power/power_supply/bd7181x_bat\x00POWER_SUPPLY_NAME=bd7181x_bat\x00ACTION=change\x00SUBSYSTEM=power_supply\x00POWER_SUPPLY_CAPACITY=98\x00"
	touchMsg   = "change@/devices/platform/zforce2.0/input/input0\x00ZForce=touch\x00NAME=\"zforce2\"\x00ACTION=change\x00SUBSYSTEM=input\x00"
)

func TestParse(t *testing.T) {
	ev, ok := Parse([]byte(pressMsg))
	if !ok {
		t.Fatal("a kernel uevent should parse")
	}
	if ev.Action != "online" || ev.Env["DRIVER"] != "bd7181x" || ev.Env["SEQNUM"] != "6082" {
		t.Errorf("parsed %+v", ev)
	}
	if ev.Path != "/devices/soc0/soc.2/2100000.aips-bus/21a0000.i2c/i2c-0/0-004b" {
		t.Errorf("path = %q", ev.Path)
	}
	for name, msg := range map[string]string{
		"empty":         "",
		"no @":          "online\x00A=b\x00",
		"udev's header": "libudev\x00\xfe\xed\xca\xfe\x00\x00\x00",
		"no action":     "@/devices/x\x00A=b\x00",
	} {
		if _, ok := Parse([]byte(msg)); ok {
			t.Errorf("%s should not parse", name)
		}
	}
	// No variables, or a trailing one with no "=", is still a message.
	if ev, ok := Parse([]byte("remove@/devices/x\x00")); !ok || ev.Action != "remove" || len(ev.Env) != 0 {
		t.Errorf("bare message: %+v ok=%v", ev, ok)
	}
}

func TestOnlyThePowerButtonIsAPress(t *testing.T) {
	for msg, want := range map[string]bool{pressMsg: true, batteryMsg: false, touchMsg: false} {
		ev, ok := Parse([]byte(msg))
		if !ok {
			t.Fatalf("did not parse: %q", msg[:20])
		}
		if got := IsPowerButton(ev); got != want {
			t.Errorf("%s: IsPowerButton = %v, want %v", ev.Path, got, want)
		}
	}
	// The same driver announcing something else is not a press.
	if ev, _ := Parse([]byte("change@/devices/x/0-004b\x00DRIVER=bd7181x\x00ACTION=change\x00")); IsPowerButton(ev) {
		t.Error("a change event from the power chip is not a press")
	}
	// A press from a chip we haven't checked is not recognised (yet).
	if ev, _ := Parse([]byte("online@/devices/x\x00DRIVER=somethingelse\x00ACTION=online\x00")); IsPowerButton(ev) {
		t.Error("an unknown driver must not count")
	}
}

func TestEventStringIsOneLine(t *testing.T) {
	ev, _ := Parse([]byte(pressMsg))
	if s := ev.String(); len(s) == 0 || s[:7] != "online@" {
		t.Errorf("String() = %q", s)
	}
}
