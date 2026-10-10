/**
 * Tobe.h - who this firmware is.
 *
 * Everything here comes from the build: build/projects.json is the one place a firmware's name and version are
 * written down, and build/scripts/tobe_pio.py turns that into these macros for every compile. Nothing in a
 * project should hard-code its own name or build number.
 *
 *   TOBE_PROJECT   the project, e.g. "EngineControl"
 *   TOBE_FW_NAME   this firmware, e.g. "EngineControl-CanSim-C3" (the project name when it has only one)
 *   FW_BUILD       its build number (a plain integer that only ever goes up)
 *
 * Every user-visible name - web page titles and headings, the WiFi network a board makes, serial banners,
 * release file names - starts with "TOBE" (takigama open boat experiments).
 */
#pragma once

#include <Arduino.h>

#ifndef FW_BUILD
#define FW_BUILD 0
#endif
#ifndef TOBE_PROJECT
#define TOBE_PROJECT "Unnamed"
#endif
#ifndef TOBE_FW_NAME
#define TOBE_FW_NAME TOBE_PROJECT
#endif

namespace tobe {

inline const char *project() { return TOBE_PROJECT; }
inline const char *fwName() { return TOBE_FW_NAME; }
inline int build() { return FW_BUILD; }

/** "TOBE EngineControl-CanSim-C3" - the name to put in a page title / heading / banner. */
String title();

/** "TOBE EngineControl-CanSim-C3 v28" */
String titleWithVersion();

/** Last two bytes of the WiFi MAC, e.g. "A4CF" - tells boards of the same kind apart. */
String macSuffix();

/** The open WiFi network a board makes in setup mode: "TOBE-<name>-A4CF", cut to fit the 32 character limit. */
String apName();

/** Restart after a short pause (lets a reply / log line leave first). */
void restart(uint32_t delay_ms = 250);

}  // namespace tobe
