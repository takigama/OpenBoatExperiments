package pages

import (
	"image"
	"testing"

	"signalkpaperdisplay/internal/render"
	"signalkpaperdisplay/internal/units"
)

func drawSettings(t *testing.T, v SettingsView) *render.Canvas {
	t.Helper()
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	Settings(c, v, units.Settings{}, false, nil, "")
	return c
}

func mid(r image.Rectangle) image.Point { return image.Pt((r.Min.X+r.Max.X)/2, (r.Min.Y+r.Max.Y)/2) }

func TestPowerScreenOffersEachChoiceOnce(t *testing.T) {
	const w = 1072
	v := SettingsView{Screen: SettingsPower}
	for i, ch := range PowerChoices {
		got := SettingsTap(v, mid(powerButton(i, w)).X, mid(powerButton(i, w)).Y, w)
		if got.Kind != ActPowerPick || got.Value != ch.ID {
			t.Errorf("button %d (%s) = %+v", i, ch.ID, got)
		}
	}
	// A tap in the gaps, and below, picks nothing - a stray touch must not.
	gap := powerButton(0, w).Max.Y + powerBtnGap/2
	if got := SettingsTap(v, 500, gap, w); got.Kind != ActNone {
		t.Errorf("a tap between the buttons = %+v", got)
	}
	if got := SettingsTap(v, 500, 1300, w); got.Kind != ActNone {
		t.Errorf("a tap below the buttons = %+v", got)
	}
	// Choosing is not doing: nothing here carries anything out.
	for i := range PowerChoices {
		if got := SettingsTap(v, mid(powerButton(i, w)).X, mid(powerButton(i, w)).Y, w); got.Kind == ActPowerDo {
			t.Errorf("a first tap must only ask, got %+v", got)
		}
	}
}

func TestPowerConfirmation(t *testing.T) {
	const w = 1072
	for _, ch := range PowerChoices {
		v := SettingsView{Screen: SettingsPowerConfirm, Power: ch.ID}
		if got := SettingsTap(v, mid(confirmYes).X, mid(confirmYes).Y, w); got.Kind != ActPowerDo || got.Value != ch.ID {
			t.Errorf("YES for %s = %+v", ch.ID, got)
		}
		if got := SettingsTap(v, mid(confirmCancel).X, mid(confirmCancel).Y, w); got.Kind != ActBack {
			t.Errorf("CANCEL for %s = %+v", ch.ID, got)
		}
		if got := SettingsTap(v, 500, 450, w); got.Kind != ActNone {
			t.Errorf("a tap on the question = %+v", got)
		}
	}
	if confirmYes.Overlaps(confirmCancel) {
		t.Error("the two buttons overlap")
	}
	// Back from the confirmation goes to the choices, and from there to the list.
	if ParentScreen(SettingsPowerConfirm) != SettingsPower || ParentScreen(SettingsPower) != SettingsRoot {
		t.Error("back should go confirmation -> choices -> list")
	}
}

func TestPowerScreensDraw(t *testing.T) {
	list := drawSettings(t, SettingsView{Screen: SettingsPower})
	for i := range PowerChoices {
		if inked(list, powerButton(i, 1072)) == 0 {
			t.Errorf("button %d is not drawn", i)
		}
	}
	for _, ch := range PowerChoices {
		v := SettingsView{Screen: SettingsPowerConfirm, Power: ch.ID}
		c := drawSettings(t, v)
		if got := inked(c, confirmYes); got < confirmYes.Dx()*confirmYes.Dy()/2 {
			t.Errorf("%s: YES should be a solid block, %d dark pixels", ch.ID, got)
		}
		if inked(c, confirmCancel) == 0 || inked(c, image.Rect(0, 280, 1072, 520)) == 0 {
			t.Errorf("%s: the question or CANCEL is missing", ch.ID)
		}
	}
	if !differs(drawSettings(t, SettingsView{Screen: SettingsPowerConfirm, Power: PowerOff}),
		drawSettings(t, SettingsView{Screen: SettingsPowerConfirm, Power: PowerRestart})) {
		t.Error("each choice should ask its own question")
	}
}

func TestEveryPowerChoiceHasItsWords(t *testing.T) {
	seen := map[string]bool{}
	for _, c := range PowerChoices {
		if c.ID == "" || c.Label == "" || c.Sub == "" || c.Question == "" || c.Detail == "" || c.Farewell == "" || c.Note == "" {
			t.Errorf("choice %+v is missing words", c)
		}
		if seen[c.ID] {
			t.Errorf("%s is offered twice", c.ID)
		}
		seen[c.ID] = true
		if got, ok := PowerChoiceByID(c.ID); !ok || got.ID != c.ID {
			t.Errorf("cannot find %s again", c.ID)
		}
	}
	if _, ok := PowerChoiceByID("nope"); ok {
		t.Error("found a choice that does not exist")
	}
	for _, id := range []string{PowerStock, PowerRestart, PowerOff} {
		if !seen[id] {
			t.Errorf("%s is not on the screen", id)
		}
	}
}

func TestFarewellSaysWhatHappened(t *testing.T) {
	for _, ch := range PowerChoices {
		c, err := render.NewCanvas(1072, 1448)
		if err != nil {
			t.Fatal(err)
		}
		Farewell(c, ch.ID)
		if inked(c, c.Bounds()) == 0 {
			t.Errorf("%s: the last picture is blank", ch.ID)
		}
	}
	c, _ := render.NewCanvas(1072, 1448)
	Farewell(c, "nope")
	if inked(c, c.Bounds()) != 0 {
		t.Error("an unknown choice should draw nothing")
	}
	if !differs(farewellOf(t, PowerOff), farewellOf(t, PowerRestart)) {
		t.Error("powering off and restarting should say different things")
	}
}

func farewellOf(t *testing.T, id string) *render.Canvas {
	c, err := render.NewCanvas(1072, 1448)
	if err != nil {
		t.Fatal(err)
	}
	Farewell(c, id)
	return c
}
