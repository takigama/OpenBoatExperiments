/**
 * TobeOta.h - over-the-air updates from GitHub Releases, the same way in every firmware.
 *
 * How it works
 *   - Every release is a GitHub Release holding the firmware file "TOBE-<name>-v<build>.bin".
 *   - A manifest (a small JSON file committed in the repo, served by raw.githubusercontent.com) says which build
 *     is current and where to get it. release.sh writes it - last, so it never points at a file that is not there.
 *   - The firmware finds ITS entry in the manifest, compares the build number with FW_BUILD, and if the manifest's
 *     is higher downloads the file, checks its MD5, and flashes it.
 *
 * Manifest layouts (read both)
 *   one firmware:   { "build": 4, "url": "https://...", "md5": "..." }
 *   several:        { "<device>": { "<variant>": { "build": ..., "url": ..., "md5": ... } } }
 * Which entry is "this" firmware, and where the manifest is, is decided by the build (build/projects.json):
 * TOBE_OTA_MANIFEST_URL, TOBE_OTA_DEVICE, TOBE_OTA_VARIANT.
 *
 * Security: the TLS connection is encrypted but the certificate chain is NOT verified (a small ESP32 cannot afford
 * the full CA bundle, and a trimmed one goes stale when GitHub rotates). The MD5 from the manifest is what
 * protects the flash from a corrupted or substituted download.
 *
 * Needs WiFi up (STA) - the caller arranges that.
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace tobe {
namespace ota {

struct Entry {
    bool valid = false;
    uint32_t build = 0;
    String url;
    String md5;
};

struct Info {
    bool ok = false;          // the manifest was fetched and this firmware has an entry in it
    bool available = false;   // ... and its build is newer than ours
    Entry entry;
    String error;             // why ok is false
};

/** The manifest this firmware reads (TOBE_OTA_MANIFEST_URL unless setManifestUrl() changed it). */
String manifestUrl();
/** Point at a different manifest, e.g. a local test server. */
void setManifestUrl(const String &url);

/** GET the manifest (https or http, redirects followed). */
bool fetchManifest(JsonDocument &doc, String *error = nullptr);

/** Find one entry in a fetched manifest. device/variant may be "" for a single-firmware manifest. */
Entry lookup(JsonDocument &doc, const char *device, const char *variant);

/** Fetch the manifest and look up THIS firmware's entry. */
Info check();

/**
 * GitHub answers a release-asset URL with a redirect to a signed URL on another host. Following it inside the
 * HTTP library opens a second TLS connection while the first is still closing, which fails on boards short of
 * contiguous RAM (the HELM). So the redirect is followed here by hand, one connection at a time, and the final
 * URL is returned. Plain http URLs are returned unchanged.
 */
String resolveUrl(const String &url);

/** Called while downloading: bytes written so far, and the total (0 if the server did not say). */
typedef void (*ProgressFn)(size_t done, size_t total);

struct ApplyOptions {
    ProgressFn progress = nullptr;
    bool reboot = true;       // restart on success
};

/**
 * Download `url`, check it against `md5` (skipped if empty), flash it to the other app slot and make it the boot
 * slot. Returns true on success (after a restart, unless opts.reboot is false). On failure nothing is changed and
 * lastError() says why.
 */
bool apply(const String &url, const String &md5, const ApplyOptions &opts = ApplyOptions());

/** check() then apply() if there is something newer. Returns true only if an update was applied. */
bool updateNow(String *message = nullptr, const ApplyOptions &opts = ApplyOptions());

const String &lastError();

/**
 * Tell the bootloader the running firmware is good (cancels an automatic roll-back, when the bootloader has that
 * enabled). Call it once the new build has shown it works - e.g. a minute after boot.
 */
void markValid();

}  // namespace ota
}  // namespace tobe
