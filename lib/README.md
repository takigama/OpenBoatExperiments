# lib/ - code shared by the ESP32 projects

Every firmware in the repo builds against these (`lib_extra_dirs = ../../lib` in its `platformio.ini`). Fix something here and every
project gets the fix on its next build. SignalKPaperDisplay (Go, runs on a Kindle) shares nothing with the ESP32 code and is not part of this.

| library | what it gives a firmware |
|---|---|
| **TobeCore** | `Tobe.h`: the firmware's name / version / "TOBE ..." titles (all come from the build). `TobeLog.h`: `tobe::console` (use it instead of `Serial`: CR LF line ends, keeps recent output for the web log) and `tobe::logf`. |
| **TobeCli** | The serial command line: Tab completes, `?` lists, history, masked secrets. Every firmware gets `MENU STATUS WIFI WIFISHOW WIFICLEAR WEBMODE UPDATE LOG REBOOT`; a project adds its own. |
| **TobeWifi** | Saved WiFi credentials (NVS), join, the open setup network `TOBE-<name>-XXXX`, and chip radio fixes (the ESP32-C3 transmit-power cap lives here, once). |
| **TobeWeb** | The page frame (dark, "TOBE <name>"), the system section every device shows (firmware update, WiFi, log, restart), and **setup mode** (`WEBMODE`): AP or join existing WiFi, serve the setup page for 10 minutes. |
| **TobeOta** | Over-the-air update from GitHub Releases: read the manifest, find this firmware, download, MD5-check, flash. |
| **TobeFleet** | The shared secret behind an ESP-NOW network (key derivation, signed frames, join handshake). Header-only; used by EngineControl. |

The libraries build on each other (TobeCli uses TobeWifi and TobeOta; TobeWeb uses TobeWifi and TobeOta; all use TobeCore). They are
meant to be used together, as a family.

## The shape of a firmware

```cpp
#include <Tobe.h>
#include <TobeCli.h>
#include <TobeWeb.h>
#include <TobeWifi.h>

static void cmdFoo(const char *args) { tobe::console.printf("foo %s\n", args); }
static const tobe::CliCommand kCommands[] = {
    { "FOO", "<thing>", "does foo to a thing", cmdFoo },
};

void setup() {
    tobe::console.begin(115200);
    tobe::logf("%s booted", tobe::titleWithVersion().c_str());

    tobe::wifi::Config w;
    w.nvsNamespace = "wifi";            // keep the namespace the project always used
    tobe::wifi::configure(w);
    tobe::cli.begin("FOO> ", kCommands, sizeof(kCommands) / sizeof(kCommands[0]));

    if (tobe::web::setupRequested()) tobe::web::runSetupMode();   // WEBMODE was typed: never returns
    tobe::wifi::begin();                                          // join, or make the setup network
}

void loop() {
    tobe::cli.tick();
    // ... the project
}
```

And on a page:

```cpp
server.send(200, "text/html", tobe::web::page("Status", myBody + tobe::web::systemSection()));
tobe::web::attach(server);      // once: the routes the system section posts to
```

## Rules

* A firmware never hard-codes its own name, version or update URL - that is `build/projects.json`.
* Anything printed for a person goes through `tobe::console` / `tobe::logf` so it also reaches the web log. Raw data streams (NMEA, hex
  frames) go straight to `Serial` instead, so they do not flood the log.
* Periodic status prints use `TOBE_CLI_LOG(...)` so they hold off while someone is typing a command.
* Saved settings keep the NVS namespace they always had, so a board keeps its WiFi across an update.
