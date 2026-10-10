/**
 * serial_cli.h - a small line editor for the serial menu: Tab completes, ? lists, arrows recall.
 *
 * CANONICAL COPY. Also duplicated (by hand) at ../can_sim/serial_cli.h - keep byte-identical.
 *
 * The sketch owns a command table (`CliCmd[]`) and a handler that runs a finished line; this file only edits
 * the line:
 *   Tab         complete the command name (several matches: extend to what they share, or list them)
 *   ?           list the commands that match what is typed (all of them on an empty line); only before the
 *               first space, so a passphrase may contain '?'
 *   Backspace   delete a character;  Ctrl-U clear the line;  Ctrl-C abandon it
 *   Up / Down   recall earlier commands (the last few; a `KEY <passphrase>` line is never remembered)
 *   after "KEY " the characters are echoed as '*', so a passphrase never appears on the screen
 * A plain program (the test logger) can still just send "COMMAND\r\n": a line is a line.
 */
#pragma once

#include <Arduino.h>

struct CliCmd {
    const char *name;   /* first word, e.g. "KEY" or "WIFI:" */
    const char *args;   /* argument hint, "" if none */
    const char *help;
};

#define CLI_HISTORY  6
#define CLI_LINE_MAX 128

static char    g_cli_hist[CLI_HISTORY][CLI_LINE_MAX];
static int     g_cli_hist_n = 0;
static int     g_cli_hist_pos = -1;          /* -1 = editing the live line */
static char    g_cli_saved[CLI_LINE_MAX];    /* the live line, while browsing history */
static uint8_t g_cli_esc = 0;                /* 1 = saw ESC, 2 = saw ESC [ */
static bool    g_cli_last_cr = false;
static uint32_t g_cli_last_input_ms = 0;   /* when a key was last pressed */

/* True for a few seconds after the last keypress: periodic status lines are held back so they do not land
 * in the middle of the line being typed. */
static bool cli_quiet(void)
{
    return g_cli_last_input_ms && millis() - g_cli_last_input_ms < 10000;
}
#define CLI_LOG(...) do { if (!cli_quiet()) Serial.printf(__VA_ARGS__); } while (0)

static bool cli_secret(const char *line, size_t len)
{
    return len >= 4 && strncasecmp(line, "KEY ", 4) == 0;   /* what follows "KEY " is a passphrase */
}

static void cli_redraw(const char *prompt, const char *line, size_t len)
{
    Serial.print("\r\x1b[K");
    Serial.print(prompt);
    if (cli_secret(line, len)) {
        Serial.write((const uint8_t *)line, 4);
        for (size_t i = 4; i < len; i++) Serial.write('*');
    } else if (len) {
        Serial.write((const uint8_t *)line, len);
    }
}

static void cli_list(const CliCmd *cmds, int n, const char *prefix, size_t plen)
{
    for (int i = 0; i < n; i++)
        if (strncasecmp(cmds[i].name, prefix, plen) == 0)
            Serial.printf("  %-11s %-18s %s\r\n", cmds[i].name, cmds[i].args, cmds[i].help);
}

static void cli_history_add(const char *line)
{
    if (g_cli_hist_n && strcmp(g_cli_hist[g_cli_hist_n - 1], line) == 0) return;
    if (g_cli_hist_n == CLI_HISTORY) {
        for (int i = 1; i < CLI_HISTORY; i++) strcpy(g_cli_hist[i - 1], g_cli_hist[i]);
        g_cli_hist_n--;
    }
    strncpy(g_cli_hist[g_cli_hist_n], line, CLI_LINE_MAX - 1);
    g_cli_hist[g_cli_hist_n][CLI_LINE_MAX - 1] = 0;
    g_cli_hist_n++;
}

static void cli_complete(const CliCmd *cmds, int n, const char *prompt, char *line, size_t cap, size_t *len)
{
    if (memchr(line, ' ', *len)) return;   /* arguments are not completed */
    int matches = 0, first = -1;
    for (int i = 0; i < n; i++) {
        if (strncasecmp(cmds[i].name, line, *len) == 0) {
            if (first < 0) first = i;
            matches++;
        }
    }
    if (matches == 0) { Serial.write(7); return; }

    if (matches == 1) {
        const char *name = cmds[first].name;
        size_t nl = strlen(name);
        if (nl + 2 < cap) {
            memcpy(line, name, nl);
            *len = nl;
            if (cmds[first].args[0] && name[nl - 1] != ':') line[(*len)++] = ' ';
        }
        cli_redraw(prompt, line, *len);
        return;
    }

    size_t lcp = strlen(cmds[first].name);   /* what all the matches share */
    for (int i = first + 1; i < n; i++) {
        if (strncasecmp(cmds[i].name, line, *len) != 0) continue;
        size_t k = 0;
        while (k < lcp && cmds[i].name[k] && tolower(cmds[i].name[k]) == tolower(cmds[first].name[k])) k++;
        lcp = k;
    }
    if (lcp > *len && lcp < cap) {
        memcpy(line, cmds[first].name, lcp);
        *len = lcp;
        cli_redraw(prompt, line, *len);
        return;
    }
    Serial.print("\r\n");
    for (int i = 0; i < n; i++)
        if (strncasecmp(cmds[i].name, line, *len) == 0) { Serial.print(cmds[i].name); Serial.print("  "); }
    Serial.print("\r\n");
    cli_redraw(prompt, line, *len);
}

/* Feed one received byte. Returns true when a full line is ready in `line` (NUL-terminated, length *len):
 * the caller runs it, sets *len = 0 and prints the prompt. */
static bool cli_feed(char c, const CliCmd *cmds, int n, const char *prompt, char *line, size_t cap, size_t *len)
{
    g_cli_last_input_ms = millis();
    if (g_cli_esc == 1) { g_cli_esc = (c == '[') ? 2 : 0; return false; }
    if (g_cli_esc == 2) {
        g_cli_esc = 0;
        if (c == 'A' && g_cli_hist_n) {            /* up: older */
            if (g_cli_hist_pos == -1) {
                memcpy(g_cli_saved, line, *len);
                g_cli_saved[*len] = 0;
                g_cli_hist_pos = g_cli_hist_n - 1;
            } else if (g_cli_hist_pos > 0) {
                g_cli_hist_pos--;
            }
            strncpy(line, g_cli_hist[g_cli_hist_pos], cap - 1);
            line[cap - 1] = 0;
            *len = strlen(line);
            cli_redraw(prompt, line, *len);
        } else if (c == 'B' && g_cli_hist_pos != -1) {   /* down: newer, then back to what was being typed */
            const char *src;
            if (g_cli_hist_pos < g_cli_hist_n - 1) src = g_cli_hist[++g_cli_hist_pos];
            else { g_cli_hist_pos = -1; src = g_cli_saved; }
            strncpy(line, src, cap - 1);
            line[cap - 1] = 0;
            *len = strlen(line);
            cli_redraw(prompt, line, *len);
        }
        return false;
    }
    if (c == 0x1b) { g_cli_esc = 1; return false; }

    if (c == '\n' && g_cli_last_cr) { g_cli_last_cr = false; return false; }   /* the LF of a CRLF */
    g_cli_last_cr = (c == '\r');

    if (c == '\r' || c == '\n') {
        Serial.print("\r\n");
        line[*len] = 0;
        if (*len && !cli_secret(line, *len)) cli_history_add(line);
        g_cli_hist_pos = -1;
        return true;
    }
    if (c == 0x03) { Serial.print("^C\r\n"); *len = 0; Serial.print(prompt); return false; }
    if (c == 0x15) { *len = 0; cli_redraw(prompt, line, 0); return false; }
    if (c == 0x08 || c == 0x7f) {
        if (*len) { (*len)--; Serial.print("\b \b"); }
        return false;
    }
    if (c == '\t') { cli_complete(cmds, n, prompt, line, cap, len); return false; }
    if (c == '?' && !memchr(line, ' ', *len)) {
        Serial.print("\r\n");
        cli_list(cmds, n, line, *len);
        cli_redraw(prompt, line, *len);
        return false;
    }
    if ((uint8_t)c < 0x20) return false;

    if (*len < cap - 1) {
        line[(*len)++] = c;
        Serial.write(cli_secret(line, *len) && *len > 4 ? '*' : (uint8_t)c);
    }
    return false;
}
