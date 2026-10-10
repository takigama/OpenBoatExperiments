/**
 * TobeCli.h - the serial command line every TOBE firmware has.
 *
 * At the prompt:
 *   Tab         complete the command name (several match: extend to what they share, or list them)
 *   ?           list the commands that match what is typed (all of them on an empty line)
 *   Backspace   delete;  Ctrl-U clear the line;  Ctrl-C abandon it
 *   Up / Down   recall earlier lines (a line of a CLI_SECRET command is never remembered)
 *   arguments of a CLI_SECRET command (a passphrase) are echoed as '*'
 * A program (a test rig) can still just send "COMMAND\r\n": a line is a line.
 *
 * Every firmware gets these without writing a line:
 *   MENU (HELP, ?)   STATUS   WIFI <ssid> [password]   WIFISHOW   WIFICLEAR   WEBMODE   UPDATE   LOG   REBOOT
 * (WIFI:<ssid>,<password> is still accepted.) A project adds its own commands, and may replace a built-in by
 * giving a command of the same name.
 *
 * Use:
 *     static void cmdFoo(const char *args) { tobe::console.printf("foo %s\n", args); }
 *     static const tobe::CliCommand kCmds[] = {
 *         { "FOO", "<thing>", "does foo to a thing", cmdFoo },
 *         { "KEY", "<passphrase>", "set the secret", cmdKey, tobe::CLI_SECRET },
 *     };
 *     setup():  tobe::cli.begin("SIM> ", kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
 *     loop():   tobe::cli.tick();
 * Periodic status prints should use TOBE_CLI_LOG(...) so they do not land in the middle of a line being typed.
 */
#pragma once

#include <Arduino.h>

#include "TobeLog.h"

namespace tobe {

typedef void (*CliHandler)(const char *args);   // args: everything after the command name, trimmed ("" if none)

enum : uint8_t {
    CLI_SECRET = 1,   // the arguments are a secret: masked on screen, kept out of the history
    CLI_HIDDEN = 2,   // runs, and completes, but is left out of the ? / MENU list
};

struct CliCommand {
    const char *name;   // one word, e.g. "KEY"
    const char *args;   // argument hint for the list, "" if none
    const char *help;
    CliHandler fn;
    uint8_t flags;      // CLI_*  (omit for 0)
};

class Cli {
public:
    /** `prompt` e.g. "SIM> ". `cmds` may be NULL. The table must outlive the CLI (make it static const). */
    void begin(const char *prompt, const CliCommand *cmds, size_t count);

    /**
     * Add more commands (a library's own table, e.g. TobeFleet's KEY commands) after begin(). A command with
     * the name of an existing one - including a built-in - replaces it. Tables must outlive the CLI.
     */
    void add(const CliCommand *cmds, size_t count);

    /** Extra lines for STATUS (called after the standard ones). */
    void setStatusHook(void (*fn)());

    /**
     * The "Type ? for the command list" line and first prompt, printed once 3 s after boot (default on).
     * Turn it off where the serial port carries data for another program: the CLI then says nothing until
     * somebody types.
     */
    void setGreeting(bool on);

    /** Call every loop(). */
    void tick();

    /** Run one line as if it had been typed. */
    void run(const char *line);

    void printMenu();

    /** True for a few seconds after a key was pressed: hold back periodic prints. */
    bool quiet() const;
};

extern Cli cli;

}  // namespace tobe

#define TOBE_CLI_LOG(...)                                       \
    do {                                                        \
        if (!tobe::cli.quiet()) tobe::console.printf(__VA_ARGS__); \
    } while (0)
