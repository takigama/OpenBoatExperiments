#pragma once

#include <Arduino.h>

// GitHub-hosted OTA - identical approach and identical code to
// FishFinderProBluetooth's ota_manager.h/.cpp (see that file's comments
// for the full "why raw manifest, why setInsecure(), why MD5" reasoning),
// just pointed at this project's own manifest path.
namespace OtaManager {

struct UpdateInfo {
    bool available = false;
    uint32_t build = 0;
    String url;
    String md5;
};

UpdateInfo checkForUpdate();
bool applyUpdate(const UpdateInfo &info);

}  // namespace OtaManager
