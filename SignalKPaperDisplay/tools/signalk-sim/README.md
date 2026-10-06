# signalk-sim: fake boat data for a SignalK server

`sim.js` is a small Node script that makes up a boat and sends it to a SignalK server once a
second, over the server's WebSocket, as deltas labelled source `sim`. It exists to exercise the
e-ink dashboard (and anything else that reads SignalK) without a boat. Nothing here is realistic
beyond "plausible enough to show the display". All values are SI, as SignalK specifies.

`run.sh` starts a throwaway SignalK server (port 3001, no security) and the simulator, both in
Docker; see its header for the commands. It recreates the two containers the dashboard was
developed against: a `signalk/signalk-server` called `signalk-kindle` and a second container,
`signalk-sim`, running `sim.js` against it.

## What it sends

**Our boat**, starting off Sydney (-33.85, 151.28) and sailing roughly north-east:
- position, true and magnetic heading, course over ground (it wanders away from the heading, like
  leeway), speed over ground (about 5 to 6 knots) and speed through water;
- wind: true wind from about 090 at about 8 m/s, slowly shifting, with the apparent wind worked out
  from it and the boat's motion (`windangles.js`, with a test in `test_windangles.js`);
- depth, rate of turn, attitude (roll, pitch, yaw), water temperature, outside temperature,
  pressure and humidity, rudder angle, autopilot state and target;
- a waypoint 3 nm away at 010, as the course API's `navigation.course.calcValues.*` (bearing,
  distance, time to go, cross-track error);
- two batteries, an engine (revolutions, temperature, oil pressure, fuel rate) and five tanks.

**Four AIS ships** (MMSI 235000001 to 235000004), each with a standing role relative to our boat so
that every kind is always on show: two closing (one near, one far) and two opening (one near, one
far). Their courses are re-aimed every second from where we are now, and a ship that has passed or
wandered off goes back to its station. Each sends its name, MMSI, position, course, speed and heading.

## Switching the GPS off

While a file called `nogps` exists in this folder, the boat's own `navigation.position` is left out and
nothing else changes (the AIS ships still send theirs). `./run.sh gps-off` and `gps-on` make and remove
it. It is for testing the dashboard's NO DATA rule, which should show once our own GPS has been silent
for the configured time even though other vessels keep reporting.

## Things worth knowing

- **A SignalK server remembers the last value from every source, forever, and replays them all to each
  client that connects**, before the live data. Anything you publish while testing (another source's
  position, a test switch) stays in that server's cache until it restarts. Against one such server
  OpenCPN lost its GPS about 20 seconds after connecting and then dead-reckoned the boat off across
  Africa; after restarting the server it was fine. We never proved which leftover did it, so treat it as
  a likely cause, not a certainty. `./run.sh reset` clears the cache.
- A fresh `signalk/signalk-server` container refuses anonymous access unless it is started with
  `--no-securityenabled`, which `run.sh` does. Do not leave a server like that on a network you do not trust.
- The script's address comes from `SK_URL` (default `ws://signalk-kindle:3000/signalk/v1/stream?subscribe=none`,
  the container name `run.sh` gives the server).
