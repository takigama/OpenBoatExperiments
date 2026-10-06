// The wind angles the fake SignalK server reports, kept apart from sim.js so
// they can be tested on their own.
//
// The true wind angle from the bow is whatever the true wind direction and
// our heading make it. The apparent wind is that angle pulled toward the bow
// by between 12 and 28 degrees (apparent wind always blows more from ahead
// than the true wind does), wandering slowly - so the two are always at least
// 10 and never more than 30 degrees apart, which is what the display's
// "show the true marker only when it differs" rule needs to be exercised.
const rad = d => (d * Math.PI) / 180;
const normPi = a => { a = ((a % (2 * Math.PI)) + 2 * Math.PI) % (2 * Math.PI); return a > Math.PI ? a - 2 * Math.PI : a; };

// twdFrom: true wind direction (radians, where it blows from); hdg: heading
// (radians); t: seconds. Returns {trueRel, awa, deltaDeg}, angles in radians
// from the bow, positive to starboard.
function windAngles(twdFrom, hdg, t) {
  const trueRel = normPi(twdFrom - hdg);
  const deltaDeg = 20 + 8 * Math.sin(t / 40);        // 12 .. 28
  const sign = trueRel >= 0 ? 1 : -1;
  const awa = normPi(trueRel - sign * rad(deltaDeg)); // toward the bow
  return { trueRel, awa, deltaDeg };
}

module.exports = { windAngles };
