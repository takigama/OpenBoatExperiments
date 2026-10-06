# signalk-test: probes and experiments for a SignalK server

Small Python scripts used while working out how SignalK behaves (sources, priorities, failover, what
a client is sent). They need `pip install websockets`. Each takes the server as `host:port`, or from
the `SK_HOST` environment variable (default `127.0.0.1:3001`).

**Several of them publish fake values into the server. Use a throwaway one** (see
`../signalk-sim/run.sh`), not a server you care about: a SignalK server keeps the last value of every
source until it restarts, so test data lingers and is replayed to every client that connects.

| Script | What it does | Writes? |
|---|---|---|
| `what_flows.py` | Listens for 12 s and summarises what the server sends: values per second, own paths grouped, other vessels, sources | no |
| `raw_shape.py` | Prints the first messages of one or more servers, with their contexts and whether updates carry timestamps | no |
| `probe_sk.py` | Subscribes to one path after `subscribe=none` and shows what comes back, and whether a ping is answered (`-p path`) | no |
| `find_everything.py` | Walks the whole data model, resources and course API for every latitude/longitude, and any number that looks like 19 or 27 | no |
| `sim_motion.py` | Measures how fast the simulated boat really moves against its reported speed | no |
| `listen_udp.py` | Listens on the usual NMEA 0183 UDP ports for broadcast position sentences and decodes them | no |
| `probe_sources.py` | Two sources publish one path; what do REST and the websocket show a client? | yes (a test path) |
| `set_priority.py` | Sets a source ranking through the admin API (`PUT /skServer/priorities`), then shows what a client now receives; `clear` removes it | yes (config) |
| `failover_timing.py` | As a client, times how long a ranked source's silence takes to fail over to the next, and how fast it switches back | yes (test paths, config) |
| `two_gps_forever.py` | Two fake GPS sources (5 nm apart) publishing constantly; `... B` publishes only B | yes |
| `pos_only.py` | Publishes only `navigation.position`, moving at 5 knots | yes |
| `relay_ctl.py` | Copies one server's live stream onto another, only the paths listed in a control file (re-read as it changes) | yes (to the target) |

## What we found with them (SignalK server 2.33)

- With **no source priorities** on a path, a client is sent every update from every source, each
  labelled with its source; the REST path shows one headline value (the latest) plus a map of all
  sources. The dashboard ignores source labels, so several GPS units on one path make its position
  jump between them: rank them.
- With a **ranking** on the path, a client subscribed to everything is sent only the winning
  source. A lower-ranked source takes over only after the winner has been silent for the lower
  source's timeout (milliseconds in the API, `PUT /skServer/priorities`); measured failover was the
  timeout plus up to one update interval (2 s timeout: 2.50 s; 5 s: 5.51 s; 10 s: 10.51 s), and
  switching back to the higher-ranked source is immediate on its next update. The only metric is time
  since the last accepted update (server receive time): no fix quality, HDOP or validity is looked at.
- `subscribe=none` followed by a subscription to one path works, delivers that path's cached value
  at once and then only changes, and `"policy": "instant", "minPeriod": 5000` throttles it.
- SignalK accepted any path and any bank name published to it (`electrical.switches.kindle.state`,
  `electrical.switches.bank.kindle.0.state`); the standard shape is
  `electrical.switches.bank.<bank>.<channel>.state`.
- See `../signalk-sim/README.md` for the OpenCPN-and-leftover-test-values lesson.

`../kindle-fb.py` (one level up) grabs a Kindle's screen over key-only ssh, for checking what the
dashboard is really showing.
