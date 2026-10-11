#include "TobeCyd.h"

#include <Preferences.h>
#include <WebServer.h>

#include <Tobe.h>
#include <TobeCli.h>
#include <TobeLog.h>
#include <TobeWeb.h>

namespace tobe {
namespace cyd {

namespace {

Settings s_settings;
Config s_cfg;
bool s_cyd2usbInverted = true;
bool s_loaded = false;
bool s_begun = false;

const char *presetName(uint8_t p) {
    switch (p) {
        case PRESET_CYD: return "CYD (ILI9341, one USB port)";
        case PRESET_CYD2USB: return "CYD2USB (ILI9341 + inversion, two USB ports)";
        case PRESET_CYD2USB_ST7789: return "CYD2USB-ST7789 (ST7789V, two USB ports)";
        default: return "custom";
    }
}

const char *controllerName(uint8_t c) { return c == CTRL_ILI9341 ? "ILI9341" : "ST7789"; }

bool parseOnOff(const char *s, bool *out) {
    if (!strcasecmp(s, "on") || !strcasecmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "true")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(s, "off") || !strcasecmp(s, "0") || !strcasecmp(s, "no") || !strcasecmp(s, "false")) {
        *out = false;
        return true;
    }
    return false;
}

void save(const Settings &s) {
    Preferences p;
    if (!p.begin(s_cfg.nvsNamespace, false)) return;
    p.putUChar("preset", s.preset);
    p.putUChar("ctrl", s.controller);
    p.putBool("invert", s.invert);
    p.putBool("bgr", s.bgr);
    p.putBool("rot", s.rotate180);
    p.end();
}

void cmdDisplay(const char *args) {
    char buf[64];
    strlcpy(buf, args, sizeof(buf));
    char *arg = strchr(buf, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ') arg++;
    } else {
        arg = buf + strlen(buf);
    }

    Settings n = s_settings;
    bool b;
    if (!buf[0]) {
        tobe::console.printf("display: %s\n", describe().c_str());
        tobe::console.print(
            "  DISPLAY CYD                 ILI9341 init, not inverted      (the original, one USB port)\n"
            "  DISPLAY CYD2USB             ILI9341 init, inverted          (two USB ports; the original recipe)\n"
            "  DISPLAY CYD2USB-ST7789      ST7789 init, not inverted       (two USB ports; the newer recipe)\n"
            "  DISPLAY CONTROLLER ILI9341|ST7789   the panel init to use    (custom; restarts)\n"
            "  DISPLAY INVERT ON|OFF       colour inversion      (custom)\n"
            "  DISPLAY BGR ON|OFF          colour order          (custom; may restart)\n"
            "  DISPLAY ROTATE ON|OFF       rotate 180 degrees    (restarts)\n"
            "  DISPLAY TEST                colour bars\n");
        return;
    }
    if (!strcasecmp(buf, "test")) {
        if (!s_cfg.test || !s_cfg.test()) tobe::console.print("no screen is up in this mode to show it on\n");
        return;
    }
    if (!strcasecmp(buf, "cyd")) {
        n = presetSettings(PRESET_CYD);
        n.rotate180 = s_settings.rotate180;
    } else if (!strcasecmp(buf, "cyd2usb")) {
        n = presetSettings(PRESET_CYD2USB);
        n.rotate180 = s_settings.rotate180;
    } else if (!strcasecmp(buf, "cyd2usb-st7789")) {
        n = presetSettings(PRESET_CYD2USB_ST7789);
        n.rotate180 = s_settings.rotate180;
    } else if (!strcasecmp(buf, "custom")) {
        n.preset = PRESET_CUSTOM;
    } else if (!strcasecmp(buf, "controller") && (!strcasecmp(arg, "ili9341") || !strcasecmp(arg, "st7789"))) {
        n.controller = !strcasecmp(arg, "st7789") ? CTRL_ST7789 : CTRL_ILI9341;
        n.preset = PRESET_CUSTOM;
    } else if (!strcasecmp(buf, "invert") && parseOnOff(arg, &b)) {
        n.invert = b;
        n.preset = PRESET_CUSTOM;
    } else if (!strcasecmp(buf, "bgr") && parseOnOff(arg, &b)) {
        n.bgr = b;
        n.preset = PRESET_CUSTOM;
    } else if (!strcasecmp(buf, "rotate") && parseOnOff(arg, &b)) {
        n.rotate180 = b;
    } else {
        tobe::console.print("DISPLAY ? lists the options - type DISPLAY with nothing after it\n");
        return;
    }
    set(n);
    tobe::console.printf("display: %s\n", describe().c_str());
}

const CliCommand kCommands[] = {
    {"DISPLAY", "[CYD|CYD2USB|CYD2USB-ST7789|CONTROLLER|INVERT|BGR|ROTATE|TEST]", "show or set how this CYD's panel is driven", cmdDisplay, 0},
};

Settings resolved(const Settings &in) {
    if (in.preset == PRESET_CUSTOM) return in;
    Settings n = presetSettings(in.preset);
    n.rotate180 = in.rotate180;
    return n;
}

bool needsRestart(const Settings &now, const Settings &next) {
    return next.rotate180 != now.rotate180 || next.controller != now.controller ||
           (next.bgr != now.bgr && !s_cfg.liveColourOrder);
}

String checked(bool on) { return on ? " checked" : ""; }

String reply(const String &msg) {
    return tobe::web::page("Display", (msg.length() ? tobe::web::note(msg) : String()) + webHtml());
}

}  // namespace

Settings presetSettings(uint8_t preset) {
    Settings s;
    s.preset = preset;
    s.controller = preset == PRESET_CYD2USB_ST7789 ? CTRL_ST7789 : CTRL_ILI9341;
    s.bgr = true;
    s.invert = preset == PRESET_CYD2USB && s_cyd2usbInverted;
    s.rotate180 = false;
    return s;
}

void setCyd2usbInverted(bool inverted) {
    s_cyd2usbInverted = inverted;
    s_loaded = false;   // the stored preset is re-derived with the new flag
    s_settings = Settings();
    s_settings.invert = inverted;
}

void load() {
    if (s_loaded) return;
    s_loaded = true;
    Preferences p;
    if (!p.begin(s_cfg.nvsNamespace, true)) return;
    if (p.isKey("preset")) {
        s_settings.preset = p.getUChar("preset", PRESET_CYD2USB);
        s_settings.controller = p.getUChar("ctrl", CTRL_ILI9341) == CTRL_ST7789 ? CTRL_ST7789 : CTRL_ILI9341;
        s_settings.invert = p.getBool("invert", s_cyd2usbInverted);
        s_settings.bgr = p.getBool("bgr", true);
        s_settings.rotate180 = p.getBool("rot", false);
        if (s_settings.preset != PRESET_CUSTOM) {  // a preset is only ever its own flags
            bool rot = s_settings.rotate180;
            s_settings = presetSettings(s_settings.preset);
            s_settings.rotate180 = rot;
        }
    }
    p.end();
}

const Settings &get() {
    load();
    return s_settings;
}

String describe() {
    load();
    String d = presetName(s_settings.preset);
    d += String(", ") + controllerName(s_settings.controller) + " init";
    d += s_settings.invert ? ", inverted" : ", not inverted";
    d += s_settings.bgr ? ", BGR" : ", RGB";
    d += s_settings.rotate180 ? ", rotated 180" : ", normal orientation";
    return d;
}

void begin(const Config &cfg) {
    s_cfg = cfg;
    s_loaded = false;
    load();
    if (!s_begun) {
        s_begun = true;
        tobe::cli.add(kCommands, sizeof(kCommands) / sizeof(kCommands[0]));
    }
    if (s_cfg.applyAtBegin && s_cfg.apply) s_cfg.apply(s_settings);
}

bool set(const Settings &in, bool restartNow) {
    load();
    Settings n = resolved(in);
    bool rotChanged = n.rotate180 != s_settings.rotate180;
    bool restart = needsRestart(s_settings, n);
    s_settings = n;
    save(n);
    tobe::logf("display: %s", describe().c_str());

    if (rotChanged && s_cfg.rotationChanged) s_cfg.rotationChanged();
    if (s_cfg.apply) s_cfg.apply(s_settings);
    if (restart && restartNow) {
        tobe::console.print("display: restarting to apply this\n");
        tobe::restart(1200);
    }
    return restart;
}

String webHtml() {
    load();
    const Settings &s = s_settings;
    String h = "<hr><h3>Display</h3>";
    h += "<p>This firmware runs on every 2.8&quot; CYD. The number of USB ports does not settle which panel a "
         "unit has: pick the board that looks right (the two-USB-port boards are seen both ways), then adjust the "
         "colour order and orientation if needed.</p>";
    h += "<p>Now: <b>" + tobe::web::escape(describe()) + "</b></p>";

    h += "<form method=POST action=/display id=dform>";
    h += "<p><label for=dp>Board</label>"
         "<select name=preset id=dp onchange=\"dpPreset()\">";
    const uint8_t order[] = {PRESET_CYD, PRESET_CYD2USB, PRESET_CYD2USB_ST7789};
    for (uint8_t id : order)
        h += "<option value=" + String(id) + (s.preset == id ? " selected" : "") + ">" + presetName(id) + "</option>";
    h += String("<option value=2") + (s.preset == PRESET_CUSTOM ? " selected" : "") + ">custom (set the next three)</option>";
    h += "</select></p>";

    h += "<p><label for=dc>Panel init</label><select name=ctrl id=dc>";
    h += String("<option value=0") + (s.controller == CTRL_ILI9341 ? " selected" : "") + ">ILI9341</option>";
    h += String("<option value=1") + (s.controller == CTRL_ST7789 ? " selected" : "") + ">ST7789</option>";
    h += "</select></p>";

    h += "<p><label><input type=checkbox name=invert id=di" + checked(s.invert) + "> Invert colours</label></p>";
    h += "<p><label><input type=checkbox name=bgr id=db" + checked(s.bgr) + "> BGR colour order (untick for RGB)</label></p>";
    h += "<p><label><input type=checkbox name=rot id=dr" + checked(s.rotate180) + "> Rotate 180&deg; (touch is calibrated again)</label></p>";

    h += "<p><button>Apply</button></p>"
         "<p class=hint>A different panel init, colour order or rotation restarts the board.</p>"
         "</form>";
    h += "<form method=POST action=/display/test><p><button>Show colour bars</button></p></form>";
    // the presets' settings, so picking one shows what it means; "custom" unlocks the three controls
    h += "<script>function dpPreset(){var p=document.getElementById('dp').value,i=document.getElementById('di'),"
         "b=document.getElementById('db'),c=document.getElementById('dc');"
         "if(p!='2'){i.checked=(p=='1');b.checked=true;c.value=(p=='3')?'1':'0'}"
         "i.disabled=b.disabled=c.disabled=(p!='2')}"
         "dpPreset();document.getElementById('dform').addEventListener('submit',function(){"
         "document.getElementById('di').disabled=document.getElementById('db').disabled="
         "document.getElementById('dc').disabled=false})</script>";
    return h;
}

void webRoutes(WebServer &server) {
    server.on("/display", HTTP_POST, [&server]() {
        load();
        Settings n = s_settings;
        n.preset = (uint8_t)constrain(server.arg("preset").toInt(), 0, 3);
        n.controller = server.arg("ctrl").toInt() == 0 ? CTRL_ILI9341 : CTRL_ST7789;
        n.invert = server.hasArg("invert");
        n.bgr = server.hasArg("bgr");
        n.rotate180 = server.hasArg("rot");
        bool restart = set(n, /*restartNow=*/false);
        server.send(200, "text/html", reply(restart ? "Saved. Restarting to apply this..." : "Applied."));
        if (restart) tobe::restart(1200);
    });
    server.on("/display/test", HTTP_POST, [&server]() {
        bool shown = s_cfg.test && s_cfg.test();
        server.send(200, "text/html",
                    reply(shown ? "Colour bars shown." : "No screen is up in this mode, so there is nothing to show them on."));
    });
}

}  // namespace cyd
}  // namespace tobe
