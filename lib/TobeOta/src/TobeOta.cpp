#include "TobeOta.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

#include "Tobe.h"
#include "TobeLog.h"

#ifndef TOBE_OTA_MANIFEST_URL
#define TOBE_OTA_MANIFEST_URL ""
#endif
#ifndef TOBE_OTA_DEVICE
#define TOBE_OTA_DEVICE ""
#endif
#ifndef TOBE_OTA_VARIANT
#define TOBE_OTA_VARIANT ""
#endif

namespace tobe {
namespace ota {

namespace {

String s_manifestUrl = TOBE_OTA_MANIFEST_URL;
String s_lastError;

bool fail(const String &why) {
    s_lastError = why;
    logf("ota: %s", why.c_str());
    return false;
}

bool isHttps(const String &url) { return url.startsWith("https://"); }

}  // namespace

String manifestUrl() { return s_manifestUrl; }
void setManifestUrl(const String &url) { s_manifestUrl = url; }
const String &lastError() { return s_lastError; }

bool fetchManifest(JsonDocument &doc, String *error) {
    auto bad = [&](const String &why) {
        fail(why);
        if (error) *error = why;
        return false;
    };
    if (s_manifestUrl.isEmpty()) return bad("no manifest URL in this build");
    if (WiFi.status() != WL_CONNECTED) return bad("no WiFi connection");

    WiFiClient plain;
    WiFiClientSecure tls;
    tls.setInsecure();   // see the header: the MD5 is the integrity guarantee
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    if (!http.begin(isHttps(s_manifestUrl) ? (NetworkClient &)tls : (NetworkClient &)plain, s_manifestUrl))
        return bad("could not start the manifest request");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return bad(String("manifest fetch failed, HTTP ") + code);
    }
    String body = http.getString();
    http.end();

    DeserializationError err = deserializeJson(doc, body);
    if (err) return bad(String("manifest is not valid JSON: ") + err.c_str());
    return true;
}

Entry lookup(JsonDocument &doc, const char *device, const char *variant) {
    Entry e;
    JsonVariantConst root = doc.as<JsonVariantConst>();
    JsonVariantConst v;
    if (!root["build"].isNull()) {
        v = root;                                        // single-firmware layout
    } else if (device && device[0]) {
        v = root[device];
        if (variant && variant[0]) v = v[variant];
    }
    if (v.isNull() || v["build"].isNull()) return e;
    e.build = v["build"] | 0;
    e.url = (const char *)(v["url"] | "");
    e.md5 = (const char *)(v["md5"] | "");
    e.valid = e.build > 0 && e.url.length() > 0;
    return e;
}

Info check() {
    Info info;
    JsonDocument doc;
    if (!fetchManifest(doc, &info.error)) return info;

    info.entry = lookup(doc, TOBE_OTA_DEVICE, TOBE_OTA_VARIANT);
    if (!info.entry.valid) {
        info.error = String("the manifest has no entry for ") + TOBE_FW_NAME;
        logf("ota: %s", info.error.c_str());
        return info;
    }
    info.ok = true;
    info.available = info.entry.build > (uint32_t)FW_BUILD;
    if (info.available)
        logf("ota: update available - build %u (running %d)", (unsigned)info.entry.build, FW_BUILD);
    else
        logf("ota: up to date (running %d, manifest has %u)", FW_BUILD, (unsigned)info.entry.build);
    return info;
}

String resolveUrl(const String &start) {
    String url = start;
    for (int hop = 0; hop < 4; hop++) {
        if (!isHttps(url)) return url;
        WiFiClientSecure c;
        c.setInsecure();
        HTTPClient h;
        h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        const char *keys[] = {"Location"};
        h.collectHeaders(keys, 1);
        if (!h.begin(c, url)) {
            logf("ota: redirect resolve - could not start the request");
            return url;
        }
        int code = h.GET();
        String loc = h.header("Location");
        logf("ota: hop %d -> HTTP %d (internal RAM %u, largest block %u)", hop, code,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        h.end();
        c.stop();
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && loc.length()) {
            url = loc;
            continue;
        }
        return url;   // 200, or an error that the download itself will report
    }
    return url;
}

bool apply(const String &url, const String &md5, const ApplyOptions &opts) {
    s_lastError = "";
    if (WiFi.status() != WL_CONNECTED) return fail("no WiFi connection");

    String real = resolveUrl(url);
    WiFiClient plain;
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);   // resolveUrl() did it
    http.setTimeout(15000);
    if (!http.begin(isHttps(real) ? (NetworkClient &)tls : (NetworkClient &)plain, real)) return fail("could not start the download");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return fail(String("download failed, HTTP ") + code);
    }
    int total = http.getSize();   // -1 when the server did not say
    logf("ota: downloading %s (%d bytes)", url.c_str(), total);

    if (!Update.begin(total > 0 ? (size_t)total : UPDATE_SIZE_UNKNOWN)) {
        http.end();
        return fail(String("cannot start the update: ") + Update.errorString());
    }
    if (md5.length() == 32) Update.setMD5(md5.c_str());   // Update.end() refuses the image if it does not match

    NetworkClient *stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t written = 0;
    uint32_t lastData = millis();
    while (http.connected() && (total < 0 || written < (size_t)total)) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastData > 20000) break;   // the server went quiet
            delay(2);
            continue;
        }
        size_t n = stream->readBytes(buf, min(avail, sizeof(buf)));
        if (Update.write(buf, n) != n) {
            http.end();
            Update.abort();
            return fail(String("flash write failed: ") + Update.errorString());
        }
        written += n;
        lastData = millis();
        if (opts.progress) opts.progress(written, total > 0 ? (size_t)total : 0);
    }
    http.end();

    if (total > 0 && written != (size_t)total) {
        Update.abort();
        return fail(String("download cut short (") + written + " of " + total + " bytes)");
    }
    if (!Update.end(true)) {
        // an MD5 mismatch lands here
        return fail(String("image rejected: ") + Update.errorString());
    }
    logf("ota: update OK%s", md5.length() == 32 ? " (MD5 verified)" : "");
    if (opts.reboot) {
        logf("ota: restarting");
        restart(500);
    }
    return true;
}

bool updateNow(String *message, const ApplyOptions &opts) {
    Info info = check();
    if (!info.ok) {
        if (message) *message = info.error;
        return false;
    }
    if (!info.available) {
        if (message) *message = String("up to date (build ") + FW_BUILD + ")";
        return false;
    }
    if (!apply(info.entry.url, info.entry.md5, opts)) {
        if (message) *message = s_lastError;
        return false;
    }
    if (message) *message = String("updated to build ") + info.entry.build;
    return true;
}

void markValid() { esp_ota_mark_app_valid_cancel_rollback(); }

}  // namespace ota
}  // namespace tobe
