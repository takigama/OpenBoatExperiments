// Run: node test_windangles.js   (exits non-zero on a violation)
const { windAngles } = require('./windangles');
const deg = r => (r * 180) / Math.PI;
const rad = d => (d * Math.PI) / 180;
const diff = (a, b) => { let d = Math.abs(a - b) % (2 * Math.PI); return d > Math.PI ? 2 * Math.PI - d : d; };

let worst = { min: 999, max: 0 }, n = 0, bad = 0;
for (let t = 0; t < 7200; t += 1) {
  // Wander the heading and wind over everything the sim can do, and well past it.
  for (const hdgDeg of [0, 20, 45, 70, 90, 180, 270, 359]) {
    for (const twdDeg of [0, 60, 90, 105, 180, 250, 359]) {
      const { trueRel, awa } = windAngles(rad(twdDeg), rad(hdgDeg), t);
      const d = deg(diff(trueRel, awa));
      worst.min = Math.min(worst.min, d); worst.max = Math.max(worst.max, d); n++;
      if (d < 10 || d > 30) { bad++; if (bad < 5) console.log('OUT OF RANGE', { t, hdgDeg, twdDeg, d }); }
    }
  }
}
console.log(`${n} samples: apparent and true wind ${worst.min.toFixed(2)} to ${worst.max.toFixed(2)} degrees apart`);
if (bad) { console.log(bad + ' outside 10-30'); process.exit(1); }
console.log('ok: always between 10 and 30 degrees apart');
