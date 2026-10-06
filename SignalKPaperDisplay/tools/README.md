# tools

Helpers that are not part of the app itself.

| | What |
|---|---|
| `kindle-probe.sh` | Read-only report of a Kindle (model, screen, touch, services) for adding a new device; see "Adding a device" in the main README |
| `fbink/` | How the FBInk binary the app draws with is built, and the prebuilt copy the installer uses |
| `kindle-fb.py` | Grabs a Kindle's screen as a PNG over key-only ssh (`kindle-fb.py out.png HOST`, or set `KINDLE_HOST`); written for the Kindle 8th generation's frame buffer (600x800, stride 608), so other models need the numbers changing |
| `signalk-sim/` | A simulator that sends made-up boat data to a SignalK server, and `run.sh` to start a throwaway server with it in Docker |
| `signalk-test/` | Probes and experiments for how a SignalK server handles sources, priorities and failover |
