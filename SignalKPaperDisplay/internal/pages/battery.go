package pages

import (
	"image"
	"strconv"

	"signalkpaperdisplay/internal/battery"
	"signalkpaperdisplay/internal/render"
)

// drawBattery draws the device battery, its middle at (cx, cy): a lightning
// bolt when on external power, a battery whose fill shows the charge, and the
// percentage. fg is the ink and bg the paper (they swap on the NO DATA
// banner). Pure black and white: it is redrawn in place under the fast
// waveform. With no reading (st nil) nothing is drawn.
func drawBattery(c *render.Canvas, cx, cy int, st *battery.Status, fg, bg uint8) {
	if st == nil {
		return
	}
	const (
		bodyW, bodyH = 64, 32
		nubW, nubH   = 6, 14
		boltW, boltH = 22, 32
		gap          = 10
		textSize     = 40.0
	)
	label := "--"
	if st.Percent >= 0 {
		label = strconv.Itoa(st.Percent) + "%"
	}
	textW := c.TextWidth(label, textSize, render.Bold)

	total := bodyW + nubW + gap + textW
	if st.Plugged {
		total += boltW + gap
	}
	x := cx - total/2

	if st.Plugged {
		bolt := []image.Point{{13, 0}, {2, 17}, {10, 17}, {6, 32}, {21, 13}, {12, 13}, {17, 0}}
		for i := range bolt {
			bolt[i] = bolt[i].Add(image.Pt(x, cy-boltH/2))
		}
		c.FillPolygon(bolt, fg)
		x += boltW + gap
	}

	body := image.Rect(x, cy-bodyH/2, x+bodyW, cy+bodyH/2)
	c.FillRect(body, fg)
	inner := body.Inset(4)
	c.FillRect(inner, bg)
	if st.Percent > 0 {
		w := inner.Dx() * min(st.Percent, 100) / 100
		w = max(w, 3) // any charge at all is visible
		c.FillRect(image.Rect(inner.Min.X, inner.Min.Y, inner.Min.X+w, inner.Max.Y), fg)
	}
	c.FillRect(image.Rect(body.Max.X, cy-nubH/2, body.Max.X+nubW, cy+nubH/2), fg)

	c.Text(body.Max.X+nubW+gap, cy+int(textSize*0.35), label, textSize, render.Bold, render.Left, fg)
}
