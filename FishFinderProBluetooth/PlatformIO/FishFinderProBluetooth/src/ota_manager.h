#pragma once

#include <Arduino.h>

// GitHub-hosted OTA: a manifest.json committed to the repo (served via
// raw.githubusercontent.com) carries {build, url, md5} for the current
// release. FW_BUILD (build_flags, platformio.ini) is a plain monotonic
// int - update available whenever manifest build > FW_BUILD. Same
// approach as Esp32RaymarineSeatalk - push to GitHub, pull via OTA.
//
// TLS is deliberately setInsecure() rather than chain-verified - see
// ota_manager.cpp's top comment for why. MD5 (from the manifest, computed
// ourselves via MD5Builder while streaming the download) carries the
// integrity guarantee instead - a corrupted or tampered-with download is
// rejected before it's ever flashed.
namespace OtaManager {

struct UpdateInfo {
    bool available = false;
    uint32_t build = 0;
    String url;
    String md5;
};

// GETs the manifest and compares against FW_BUILD. Network op - only
// meaningful with a real route to github.com.
UpdateInfo checkForUpdate();

// Downloads+flashes UpdateInfo::url, verifying against its md5 before
// accepting. Reboots on success; returns (does not reboot) on failure so
// the caller can report the error.
bool applyUpdate(const UpdateInfo &info);

}  // namespace OtaManager
