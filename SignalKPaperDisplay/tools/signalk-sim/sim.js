// Fake SignalK data feed for testing the e-ink dashboard. Sends deltas at
// 1 Hz over the server's WebSocket: own-vessel navigation/wind/depth plus a
// handful of moving AIS targets. All values SI (m/s, radians, metres), per
// the SignalK spec. Throwaway - nothing here is meant to be realistic
// beyond "plausible enough to exercise the display".
const WebSocket = require('ws');
const fs = require('fs');
// While the file /sim/nogps exists the boat's own GPS position is not sent (a test of the dashboard's NO DATA rule);
// everything else, including the AIS targets, carries on.
const ownFilter = vals => (fs.existsSync('/sim/nogps') ? vals.filter(v => v.path !== 'navigation.position') : vals);
const { windAngles } = require('./windangles');

const URL = process.env.SK_URL || 'ws://signalk-kindle:3000/signalk/v1/stream?subscribe=none';
const rad = d => (d * Math.PI) / 180;
const deg = r => (r * 180) / Math.PI;
const norm2pi = a => ((a % (2 * Math.PI)) + 2 * Math.PI) % (2 * Math.PI);
const normPi = a => { a = norm2pi(a); return a > Math.PI ? a - 2 * Math.PI : a; };
const M_PER_DEG = 111320;
const NM = 1852;

function move(p, hdgRad, dist) {
  return {
    lat: p.lat + (dist * Math.cos(hdgRad)) / M_PER_DEG,
    lon: p.lon + (dist * Math.sin(hdgRad)) / (M_PER_DEG * Math.cos(rad(p.lat))),
  };
}
function pointAt(p, bearingDeg, rangeM) { return move(p, rad(bearingDeg), rangeM); }

const own = { pos: { lat: -33.85, lon: 151.28 } };
// A waypoint 3 nm away at 010 from the start, for the course data.
const waypoint = pointAt(own.pos, 10, 3 * NM);

// Four ships with a standing role relative to us, so the compass and the AIS
// boxes always have each kind to show: two closing (one near, one far) and two
// opening (one near, one far). Their courses are re-aimed every second from
// where we are NOW, so we can wander about and the roles still hold.
//   toward: steers at us, a few degrees off so it passes rather than collides
//   away:   steers directly away from us
const targets = [
  { mmsi: '235000001', name: 'Triteia',          role: 'toward', bearing: 20,  rangeNm: 0.9, offsetDeg: 6,  sog: 4 }, // closing, near
  { mmsi: '235000002', name: 'Aura',             role: 'toward', bearing: 330, rangeNm: 3.5, offsetDeg: -8, sog: 6 }, // closing, far
  { mmsi: '235000003', name: 'Traviata',         role: 'away',   bearing: 190, rangeNm: 1.0, offsetDeg: 10, sog: 5 }, // opening, near
  { mmsi: '235000004', name: 'To Keel a Sunset', role: 'away',   bearing: 120, rangeNm: 3.0, offsetDeg: -10, sog: 7 }, // opening, far
].map(t => ({ ...t, cog: 0, pos: pointAt(own.pos, t.bearing, t.rangeNm * NM) }));

function reseed(tg) {
  tg.pos = pointAt(own.pos, tg.bearing, tg.rangeNm * NM);
}

function delta(context, values) {
  return JSON.stringify({
    context,
    updates: [{ source: { label: 'sim', type: 'sim' }, timestamp: new Date().toISOString(), values }],
  });
}

function distBearing(a, b) {
  const dN = (b.lat - a.lat) * M_PER_DEG;
  const dE = (b.lon - a.lon) * M_PER_DEG * Math.cos(rad(a.lat));
  return { dist: Math.hypot(dN, dE), bearing: norm2pi(Math.atan2(dE, dN)) };
}

let t0 = Date.now();
function tick(ws) {
  const t = (Date.now() - t0) / 1000;

  // Own boat: heading wanders +-25 deg around 045 over ~3 min, speed ~5.4 kn.
  const hdg = rad(45 + 25 * Math.sin(t / 30));
  const sog = 2.8 + 0.3 * Math.sin(t / 17);
  own.pos = move(own.pos, hdg, sog);

  // Wind: true wind from ~090 at ~8 m/s, slowly shifting. Apparent derived
  // from true minus boat velocity, so it stays physically consistent.
  const twdFrom = rad(90 + 15 * Math.sin(t / 90));
  const tws = 8 + 2 * Math.sin(t / 45);
  const toward = twdFrom + Math.PI;
  const ae = tws * Math.sin(toward) - sog * Math.sin(hdg);
  const an = tws * Math.cos(toward) - sog * Math.cos(hdg);
  const aws = Math.hypot(ae, an);
  const awdFrom = Math.atan2(-ae, -an);
  // Apparent wind angle: the true wind angle pulled 12-28 degrees toward the bow.
  const { trueRel, awa } = windAngles(twdFrom, hdg, t);

  const wpb = distBearing(own.pos, waypoint);
  const depth = 12 + 6 * Math.sin(t / 60) + (Math.random() - 0.5) * 0.4;

  ws.send(delta('vessels.self', ownFilter([
    { path: 'navigation.position', value: { latitude: own.pos.lat, longitude: own.pos.lon } },
    { path: 'navigation.headingTrue', value: norm2pi(hdg) },
    { path: 'navigation.headingMagnetic', value: norm2pi(hdg - rad(12)) },
    // Leeway/current: the boat goes a few degrees off where it points, and
    // that drifts, so COG and heading visibly differ.
    { path: 'navigation.courseOverGroundTrue', value: norm2pi(hdg + rad(12) * Math.sin(t / 25)) },
    { path: 'navigation.speedOverGround', value: sog },
    { path: 'navigation.speedThroughWater', value: sog * 0.97 },
    { path: 'environment.wind.speedApparent', value: aws },
    { path: 'environment.wind.angleApparent', value: awa },
    { path: 'environment.wind.speedTrue', value: tws },
    { path: 'environment.wind.directionTrue', value: norm2pi(twdFrom) },
    { path: 'environment.depth.belowTransducer', value: depth },
    // Next waypoint, as the v2 course API's calcValues report it.
    { path: 'navigation.course.calcValues.bearingTrue', value: wpb.bearing },
    { path: 'navigation.course.calcValues.distance', value: wpb.dist },
    { path: 'navigation.course.calcValues.timeToGo', value: wpb.dist / Math.max(sog, 0.1) },
    // Water temperature (K) and two fuel tanks (ratio 0..1), like a real
    // boat reports them: slowly changing.
    { path: 'environment.water.temperature', value: 297.15 + 1.2 * Math.sin(t / 120) },
    // The rest of what a Nav box can show: rate of turn, attitude, weather,
    // steering, cross-track error, batteries, an engine, other tanks.
    { path: 'navigation.rateOfTurn', value: rad(25) * Math.cos(t / 30) / 30 },
    { path: 'navigation.attitude', value: { roll: rad(8 * Math.sin(t / 6)), pitch: rad(2 * Math.sin(t / 9)), yaw: norm2pi(hdg) } },
    { path: 'environment.outside.temperature', value: 293.15 + 2 * Math.sin(t / 300) },
    { path: 'environment.outside.pressure', value: 101300 + 200 * Math.sin(t / 500) },
    { path: 'environment.outside.humidity', value: 0.62 + 0.05 * Math.sin(t / 200) },
    { path: 'steering.rudderAngle', value: rad(6 * Math.sin(t / 12)) },
    { path: 'steering.autopilot.state', value: 'auto' },
    { path: 'steering.autopilot.target.headingTrue', value: norm2pi(hdg + rad(3)) },
    { path: 'navigation.course.calcValues.crossTrackError', value: 40 * Math.sin(t / 40) },
    { path: 'electrical.batteries.house.voltage', value: 12.6 + 0.3 * Math.sin(t / 60) },
    { path: 'electrical.batteries.house.current', value: -8 + 3 * Math.sin(t / 20) },
    { path: 'electrical.batteries.house.capacity.stateOfCharge', value: Math.max(0.2, 0.9 - t / 20000) },
    { path: 'electrical.batteries.starter.voltage', value: 12.9 },
    { path: 'propulsion.main.revolutions', value: 28 + 2 * Math.sin(t / 10) },
    { path: 'propulsion.main.temperature', value: 355 + 3 * Math.sin(t / 50) },
    { path: 'propulsion.main.oilPressure', value: 300000 + 20000 * Math.sin(t / 30) },
    { path: 'propulsion.main.fuel.rate', value: 0.0000014 },
    { path: 'tanks.freshWater.0.currentLevel', value: Math.max(0.05, 0.7 - t / 40000) },
    { path: 'tanks.wasteWater.0.currentLevel', value: 0.3 },
    { path: 'tanks.blackWater.0.currentLevel', value: 0.15 },
    { path: 'tanks.fuel.0.currentLevel', value: Math.max(0.05, 0.82 - t / 20000) },
    { path: 'tanks.fuel.1.currentLevel', value: Math.max(0.05, 0.43 - t / 30000) },
  ])));

  for (const tg of targets) {
    // Aim from where we are now: at us (toward) or straight away from us (away).
    const db = distBearing(own.pos, tg.pos); // from us to it
    const course = tg.role === 'toward' ? db.bearing + Math.PI + rad(tg.offsetDeg) : db.bearing + rad(tg.offsetDeg);
    tg.cog = deg(norm2pi(course));
    tg.pos = move(tg.pos, norm2pi(course), tg.sog);
    // Ships that have passed us or wandered off go back to their starting
    // station, so each role is always represented.
    const d2 = distBearing(own.pos, tg.pos).dist;
    if ((tg.role === 'toward' && d2 < 0.12 * NM) || (tg.role === 'away' && d2 > 6 * NM)) reseed(tg);
    ws.send(delta(`vessels.urn:mrn:imo:mmsi:${tg.mmsi}`, [
      { path: '', value: { name: tg.name } },
      { path: '', value: { mmsi: tg.mmsi } },
      { path: 'navigation.position', value: { latitude: tg.pos.lat, longitude: tg.pos.lon } },
      { path: 'navigation.courseOverGroundTrue', value: rad(tg.cog) },
      { path: 'navigation.speedOverGround', value: tg.sog },
      { path: 'navigation.headingTrue', value: rad(tg.cog) },
    ]));
  }

  if (Math.round(t) % 30 === 0) {
    console.log(`t=${Math.round(t)}s hdg=${Math.round(deg(hdg))} sog=${sog.toFixed(1)} depth=${depth.toFixed(1)} aws=${aws.toFixed(1)} awa=${Math.round(deg(awa))} twa=${Math.round(deg(trueRel))}`);
  }
}

function connect() {
  const ws = new WebSocket(URL);
  let timer;
  ws.on('open', () => { console.log('connected to', URL); timer = setInterval(() => tick(ws), 1000); });
  ws.on('error', e => console.log('ws error:', e.message));
  ws.on('close', () => { clearInterval(timer); console.log('closed, retrying in 3s'); setTimeout(connect, 3000); });
}
connect();
