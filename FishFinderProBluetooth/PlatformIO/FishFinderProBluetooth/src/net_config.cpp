#include "net_config.h"

#include <Preferences.h>

#include "debug_log.h"

namespace NetConfig {

namespace {

constexpr const char *kPrefsNamespace = "net";

String s_mqttHost;
uint16_t s_mqttPort = 1883;
String s_mqttBaseTopic = "fishfinder";

Preferences prefs() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    return p;
}

}  // namespace

void begin() {
    Preferences p = prefs();
    s_mqttHost = p.getString("host", "");
    s_mqttPort = p.getUShort("port", 1883);
    s_mqttBaseTopic = p.getString("base", "fishfinder");
    p.end();
}

String mqttHost() { return s_mqttHost; }
uint16_t mqttPort() { return s_mqttPort; }
String mqttBaseTopic() { return s_mqttBaseTopic; }

void saveMqtt(const String &host, uint16_t port, const String &baseTopic) {
    Preferences p = prefs();
    p.putString("host", host);
    p.putUShort("port", port);
    p.putString("base", baseTopic);
    p.end();

    s_mqttHost = host;
    s_mqttPort = port;
    s_mqttBaseTopic = baseTopic.isEmpty() ? "fishfinder" : baseTopic;
    DebugLog::logf("net: mqtt config saved (%s:%u, base \"%s\")", s_mqttHost.c_str(), s_mqttPort,
                    s_mqttBaseTopic.c_str());
}

}  // namespace NetConfig
