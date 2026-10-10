#include "TobeCli.h"

#include <WiFi.h>
#include <esp_system.h>

#include "Tobe.h"
#include "TobeOta.h"
#include "TobeWifi.h"

namespace tobe {

Cli cli;

namespace {

constexpr int kHistory = 6;
constexpr size_t kLineMax = 160;
constexpr int kMaxCommands = 48;

const char *s_prompt = "TOBE> ";
void (*s_statusHook)() = nullptr;

// the merged list (project commands first, then the built-ins they did not replace)
const CliCommand *s_cmds[kMaxCommands];
int s_nCmds = 0;
int s_nProject = 0;   // the first s_nProject entries are the project's, the rest the built-ins

char s_line[kLineMax];
size_t s_len = 0;
char s_hist[kHistory][kLineMax];
int s_histN = 0;
int s_histPos = -1;          // -1 = editing the live line
char s_saved[kLineMax];      // the live line while browsing history
uint8_t s_esc = 0;           // 1 = saw ESC, 2 = saw ESC [
bool s_lastCr = false;
uint32_t s_lastInputMs = 0;
bool s_greeted = false;
bool s_greeting = true;

// ------------------------------------------------------------------ helpers

const CliCommand *find(const char *name, size_t len) {
    for (int i = 0; i < s_nCmds; i++)
        if (strlen(s_cmds[i]->name) == len && strncasecmp(s_cmds[i]->name, name, len) == 0) return s_cmds[i];
    return nullptr;
}

/** The command a typed line starts with, and its secret-ness (decided on what is typed so far). */
const CliCommand *commandOf(const char *line, size_t len) {
    size_t n = 0;
    while (n < len && line[n] != ' ') n++;
    if (n == len) return nullptr;   // still typing the name
    return find(line, n);
}

bool isSecret(const char *line, size_t len) {
    const CliCommand *c = commandOf(line, len);
    return c && (c->flags & CLI_SECRET);
}

size_t secretFrom(const char *line, size_t len) {   // index where the masked part begins
    size_t n = 0;
    while (n < len && line[n] != ' ') n++;
    return n + 1;
}

void redraw() {
    console.print("\r\x1b[K");
    console.print(s_prompt);
    if (s_len && isSecret(s_line, s_len)) {
        size_t from = secretFrom(s_line, s_len);
        console.write((const uint8_t *)s_line, from);
        for (size_t i = from; i < s_len; i++) console.write('*');
    } else if (s_len) {
        console.write((const uint8_t *)s_line, s_len);
    }
}

void listMatching(const char *prefix, size_t plen) {
    for (int i = 0; i < s_nCmds; i++) {
        if (s_cmds[i]->flags & CLI_HIDDEN) continue;
        if (strncasecmp(s_cmds[i]->name, prefix, plen) == 0)
            console.printf("  %-11s %-22s %s\n", s_cmds[i]->name, s_cmds[i]->args, s_cmds[i]->help);
    }
}

void histAdd(const char *line) {
    if (s_histN && strcmp(s_hist[s_histN - 1], line) == 0) return;
    if (s_histN == kHistory) {
        for (int i = 1; i < kHistory; i++) strcpy(s_hist[i - 1], s_hist[i]);
        s_histN--;
    }
    strncpy(s_hist[s_histN], line, kLineMax - 1);
    s_hist[s_histN][kLineMax - 1] = 0;
    s_histN++;
}

void complete() {
    if (memchr(s_line, ' ', s_len)) return;   // arguments are not completed
    int matches = 0, first = -1;
    for (int i = 0; i < s_nCmds; i++) {
        if (strncasecmp(s_cmds[i]->name, s_line, s_len) == 0) {
            if (first < 0) first = i;
            matches++;
        }
    }
    if (matches == 0) {
        console.write(7);
        return;
    }
    if (matches == 1) {
        const char *name = s_cmds[first]->name;
        size_t nl = strlen(name);
        if (nl + 2 < kLineMax) {
            memcpy(s_line, name, nl);
            s_len = nl;
            if (s_cmds[first]->args[0]) s_line[s_len++] = ' ';
        }
        redraw();
        return;
    }
    size_t lcp = strlen(s_cmds[first]->name);   // what all the matches share
    for (int i = first + 1; i < s_nCmds; i++) {
        if (strncasecmp(s_cmds[i]->name, s_line, s_len) != 0) continue;
        size_t k = 0;
        while (k < lcp && s_cmds[i]->name[k] && tolower(s_cmds[i]->name[k]) == tolower(s_cmds[first]->name[k])) k++;
        lcp = k;
    }
    if (lcp > s_len && lcp < kLineMax) {
        memcpy(s_line, s_cmds[first]->name, lcp);
        s_len = lcp;
        redraw();
        return;
    }
    console.print("\r\n");
    for (int i = 0; i < s_nCmds; i++)
        if (strncasecmp(s_cmds[i]->name, s_line, s_len) == 0) {
            console.print(s_cmds[i]->name);
            console.print("  ");
        }
    console.print("\r\n");
    redraw();
}

void setLine(const char *src) {
    strncpy(s_line, src, kLineMax - 1);
    s_line[kLineMax - 1] = 0;
    s_len = strlen(s_line);
    redraw();
}

/** One argument off the front of *p: handles "quoted words". Returns false when nothing is left. */
bool nextArg(const char **p, String &out) {
    const char *s = *p;
    while (*s == ' ') s++;
    if (!*s) return false;
    out = "";
    if (*s == '"') {
        s++;
        while (*s && *s != '"') out += *s++;
        if (*s == '"') s++;
    } else {
        while (*s && *s != ' ') out += *s++;
    }
    *p = s;
    return true;
}

const char *resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "CRASH (panic)";
        case ESP_RST_INT_WDT: return "CRASH (interrupt watchdog)";
        case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
        case ESP_RST_WDT: return "CRASH (watchdog)";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_EXT: return "reset pin";
        default: return "other";
    }
}

// ------------------------------------------------------------------ built-in commands

void cmdMenu(const char *) { cli.printMenu(); }

void cmdStatus(const char *) {
    console.printf("%s\n", titleWithVersion().c_str());
    console.printf("  chip %s rev %d, %d core(s), %u MHz; last reset: %s\n", ESP.getChipModel(),
                   (int)ESP.getChipRevision(), (int)ESP.getChipCores(), (unsigned)ESP.getCpuFreqMHz(), resetReason());
    console.printf("  up %lus, free heap %u (lowest %u)\n", (unsigned long)(millis() / 1000),
                   (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    if (wifi::hasSaved()) console.printf("  WiFi: saved network \"%s\"", wifi::savedSsid().c_str());
    else console.print("  WiFi: no saved network");
    if (WiFi.status() == WL_CONNECTED)
        console.printf(", joined, IP %s, %d dBm\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else if (wifi::mode() == wifi::Mode::AP)
        console.printf(", setup network \"%s\" up at %s\n", wifi::apSsid().c_str(), wifi::ip().c_str());
    else
        console.print(", not joined\n");
    if (s_statusHook) s_statusHook();
}

void cmdWifi(const char *args) {
    const char *p = args;
    String ssid, pass;
    if (!nextArg(&p, ssid)) {
        console.print("usage: WIFI <ssid> [password]   (put a name or password with spaces in \"quotes\")\n");
        return;
    }
    nextArg(&p, pass);
    wifi::saveAndReboot(ssid, pass);
}

void cmdWifiShow(const char *) {
    if (!wifi::hasSaved()) {
        console.print("no WiFi network saved\n");
        return;
    }
    console.printf("saved network: \"%s\" (password %s)\n", wifi::savedSsid().c_str(),
                   wifi::savedPassword().length() ? "set" : "none");
}

void cmdWifiClear(const char *) {
    wifi::clearSaved();
    console.print("saved WiFi network forgotten - restarting\n");
    restart(300);
}

void cmdWebmode(const char *) {
    wifi::requestSetupMode();
    console.print("restarting into setup mode (a web page for WiFi and settings, 10 minutes)\n");
    restart(300);
}

void cmdUpdate(const char *) {
    if (WiFi.status() != WL_CONNECTED) {
        console.print("joining the saved WiFi network...\n");
        if (!wifi::joinSaved()) {
            console.print("cannot update: no WiFi connection (set one with WIFI <ssid> <password>)\n");
            return;
        }
    }
    static int lastPct = -1;
    lastPct = -1;
    ota::ApplyOptions opts;
    opts.progress = [](size_t done, size_t total) {
        if (!total) return;
        int pct = (int)(done * 100 / total) / 10 * 10;
        if (pct != lastPct) {
            lastPct = pct;
            console.printf("  %d%%\n", pct);
        }
    };
    String msg;
    if (ota::updateNow(&msg, opts)) return;   // restarts on success
    console.printf("%s\n", msg.c_str());
}

void cmdLog(const char *) {
    char *buf = (char *)malloc(TOBE_LOG_SIZE + 1);
    if (!buf) {
        console.print("out of memory\n");
        return;
    }
    size_t n = logCopy(buf, TOBE_LOG_SIZE + 1);
    console.setCapture(false);   // do not log the dump of the log
    console.write((const uint8_t *)buf, n);
    console.setCapture(true);
    free(buf);
    console.print("\n");
}

void cmdReboot(const char *) {
    console.print("restarting\n");
    restart(200);
}

const CliCommand kBuiltins[] = {
    {"MENU", "", "show this list (also HELP or ?)", cmdMenu, 0},
    {"STATUS", "", "firmware, chip, memory, WiFi", cmdStatus, 0},
    {"WIFI", "<ssid> [password]", "save a WiFi network and restart (also WIFI:<ssid>,<password>)", cmdWifi, 0},
    {"WIFISHOW", "", "the saved WiFi network", cmdWifiShow, 0},
    {"WIFICLEAR", "", "forget the saved WiFi network, restart", cmdWifiClear, 0},
    {"WEBMODE", "", "restart into setup mode: a web page for WiFi and settings (10 minutes)", cmdWebmode, 0},
    {"UPDATE", "", "check GitHub for a newer firmware and install it", cmdUpdate, 0},
    {"LOG", "", "show the recent log", cmdLog, 0},
    {"REBOOT", "", "restart", cmdReboot, 0},
};

}  // namespace

// ------------------------------------------------------------------ public

void Cli::begin(const char *prompt, const CliCommand *cmds, size_t count) {
    s_prompt = prompt ? prompt : "TOBE> ";
    s_nCmds = 0;
    s_nProject = 0;
    for (size_t i = 0; i < sizeof(kBuiltins) / sizeof(kBuiltins[0]) && s_nCmds < kMaxCommands; i++)
        s_cmds[s_nCmds++] = &kBuiltins[i];
    if (cmds && count) add(cmds, count);
}

void Cli::add(const CliCommand *cmds, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const CliCommand *c = &cmds[i];
        // a command of the same name replaces the existing one (project or built-in)
        int at = -1;
        for (int j = 0; j < s_nCmds; j++)
            if (strcasecmp(s_cmds[j]->name, c->name) == 0) { at = j; break; }
        if (at >= 0) {
            if (at < s_nProject) { s_cmds[at] = c; continue; }   // replace in place within the project part
            // a built-in: drop it, then fall through to insert into the project part
            for (int j = at; j + 1 < s_nCmds; j++) s_cmds[j] = s_cmds[j + 1];
            s_nCmds--;
        }
        if (s_nCmds >= kMaxCommands) continue;
        for (int j = s_nCmds; j > s_nProject; j--) s_cmds[j] = s_cmds[j - 1];   // project commands list first
        s_cmds[s_nProject++] = c;
        s_nCmds++;
    }
}

void Cli::setStatusHook(void (*fn)()) { s_statusHook = fn; }

void Cli::setGreeting(bool on) { s_greeting = on; }

bool Cli::quiet() const { return s_lastInputMs && millis() - s_lastInputMs < 10000; }

void Cli::printMenu() {
    console.printf("%s - Tab completes a command, ? lists them (case-insensitive):\n", titleWithVersion().c_str());
    listMatching("", 0);
}

void Cli::run(const char *line) {
    // trim
    while (*line == ' ') line++;
    if (!*line) return;
    if (strcmp(line, "?") == 0 || strcasecmp(line, "HELP") == 0) {
        printMenu();
        return;
    }
    // WIFI:<ssid>,<password> - the older spelling, still accepted
    if (strncasecmp(line, "WIFI:", 5) == 0) {
        const char *rest = line + 5;
        const char *comma = strchr(rest, ',');
        String ssid = comma ? String(rest).substring(0, comma - rest) : String(rest);
        String pass = comma ? String(comma + 1) : String();
        if (ssid.isEmpty()) console.print("WIFI: needs a network name\n");
        else wifi::saveAndReboot(ssid, pass);
        return;
    }
    size_t n = 0;
    while (line[n] && line[n] != ' ') n++;
    const CliCommand *c = find(line, n);
    if (!c) {
        console.print("unrecognized command - type ? for the list\n");
        return;
    }
    const char *args = line + n;
    while (*args == ' ') args++;
    c->fn(args);
}

void Cli::tick() {
    if (!s_greeted && s_greeting && millis() > 3000) {   // once, after the boot messages
        s_greeted = true;
        console.printf("\nType ? for the command list; Tab completes.\n%s", s_prompt);
    }
    while (console.available()) {
        char c = (char)console.read();
        s_lastInputMs = millis();

        if (s_esc == 1) { s_esc = (c == '[') ? 2 : 0; continue; }
        if (s_esc == 2) {
            s_esc = 0;
            if (c == 'A' && s_histN) {            // up: older
                if (s_histPos == -1) {
                    memcpy(s_saved, s_line, s_len);
                    s_saved[s_len] = 0;
                    s_histPos = s_histN - 1;
                } else if (s_histPos > 0) {
                    s_histPos--;
                }
                setLine(s_hist[s_histPos]);
            } else if (c == 'B' && s_histPos != -1) {   // down: newer, then back to what was being typed
                if (s_histPos < s_histN - 1) setLine(s_hist[++s_histPos]);
                else { s_histPos = -1; setLine(s_saved); }
            }
            continue;
        }
        if (c == 0x1b) { s_esc = 1; continue; }

        if (c == '\n' && s_lastCr) { s_lastCr = false; continue; }   // the LF of a CRLF
        s_lastCr = (c == '\r');

        if (c == '\r' || c == '\n') {
            console.print("\r\n");
            s_line[s_len] = 0;
            if (s_len && !isSecret(s_line, s_len)) histAdd(s_line);
            s_histPos = -1;
            if (s_len > 0) {
                static char work[kLineMax];
                memcpy(work, s_line, s_len + 1);
                s_len = 0;
                run(work);
            }
            s_len = 0;
            console.print(s_prompt);
            continue;
        }
        if (c == 0x03) { console.print("^C\r\n"); s_len = 0; console.print(s_prompt); continue; }
        if (c == 0x15) { s_len = 0; redraw(); continue; }
        if (c == 0x08 || c == 0x7f) {
            if (s_len) { s_len--; console.print("\b \b"); }
            continue;
        }
        if (c == '\t') { complete(); continue; }
        if (c == '?' && !memchr(s_line, ' ', s_len)) {   // only before the first space: a passphrase may contain '?'
            console.print("\r\n");
            listMatching(s_line, s_len);
            redraw();
            continue;
        }
        if ((uint8_t)c < 0x20) continue;

        if (s_len < kLineMax - 1) {
            s_line[s_len++] = c;
            bool masked = isSecret(s_line, s_len) && (s_len - 1) >= secretFrom(s_line, s_len);   // the space itself is shown
            console.write(masked ? (uint8_t)'*' : (uint8_t)c);
        }
    }
}

}  // namespace tobe
