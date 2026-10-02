#!/bin/sh
# Keeps paperdisplay running on the Kindle. Meant to be run from cron once a
# minute (e.g. add one line to your existing /usr/bin/startssh.sh):
#
#     /mnt/us/signalk/startpaper.sh
#
# It is idempotent - safe to run as often as you like - and does this:
#   * keeps the screen from sleeping (re-asserted every run, it resets on reboot)
#   * if the app isn't running, takes the screen from the stock UI and starts it
#   * counts crashes; after 3 in a row it rolls back to the previous version
#     (paperdisplay.prev, kept by the updater), after 5 it gives up for a while
#     and brings the stock Kindle UI back
#   * `touch /mnt/us/signalk/disable` stops the app and restores the stock UI
#     within a minute; delete the file to start again
#   * `touch /mnt/us/signalk/pause` makes the launcher do nothing at all
#     (no start, no restore) until the file is deleted
#
# Settings live in /mnt/us/signalk/launcher.conf (see launcher.conf.example).
# Plain POSIX sh: the Kindle has busybox ash, not bash.

# Cron's PATH is minimal (no /sbin, where initctl/start/stop live). Add the
# system directories as a fallback, after whatever the caller already has.
PATH="$PATH:/sbin:/usr/sbin:/bin:/usr/bin"

# Overridable only so the test script can run this against stand-ins.
DIR="${DIR:-/mnt/us/signalk}"
STATE="${STATE:-/var/tmp/paperdisplay}"   # tmpfs: crash counts reset on reboot

BIN="$DIR/paperdisplay"
CONF="$DIR/launcher.conf"
LOG="$DIR/paperdisplay.log"
DISABLE="$DIR/disable"

ROLLBACK_AFTER=3      # crashes within WINDOW before trying the previous version
GIVE_UP_AFTER=5       # crashes within WINDOW before restoring the stock UI
WINDOW=600            # seconds

mkdir -p "$STATE"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') launcher: $*" >> "$LOG"; }

running() { initctl status "$1" 2>/dev/null | grep -q 'start/running'; }

# Hands the screen back: starts every job take_screen stopped, in the reverse
# order (lab126_gui has to be up before the framework that sits on it).
restore_ui() {
  lipc-set-prop com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1
  reversed=""
  for job in $STOP_JOBS; do reversed="$job $reversed"; done
  for job in $reversed; do
    running "$job" || start "$job" >/dev/null 2>&1
  done
}

take_screen() {
  # powerd forgets this across reboots, so it is set whenever the app is
  # (re)started - which a reboot always causes - and not on every run. Run
  # from cron once a minute, lipc-set-prop (a D-Bus round trip to powerd) was
  # a noticeable stall on the Kindle's CPU right on the minute.
  lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1
  lipc-set-prop com.lab126.pillow disableEnablePillow disable >/dev/null 2>&1
  for job in $STOP_JOBS; do
    running "$job" && stop "$job" >/dev/null 2>&1
  done
}

[ -f "$CONF" ] && . "$CONF"
# lab126_gui as well as the framework: with only the framework stopped, the
# Paperwhite 3's stock UI still painted its clock over our header every minute,
# and stopping lab126_gui is what made that stop (pillow, the status bar
# daemon, turned out not to be it).
STOP_JOBS="${STOP_JOBS:-framework lab126_gui}"

# --- ssh keep-alive -----------------------------------------------------------
# This used to be a separate cron job (startssh.sh) that, every minute, set
# powerd's screensaver property, ran iptables and started a new dropbear that
# immediately failed to bind the port the running one held. Three process
# launches and a D-Bus round trip, on the minute, for nothing. Here the check
# that matters is free - it only reads files with shell builtins - and the
# rest runs when there is something to do.
#
# Settings (launcher.conf): KEEP_SSH=0 turns this off; SSH_PORT, DROPBEAR (the
# binary) and SSH_PIDFILE say where things are.
KEEP_SSH="${KEEP_SSH:-1}"
SSH_PORT="${SSH_PORT:-2223}"
DROPBEAR="${DROPBEAR:-/mnt/us/koreader/dropbear}"
SSH_PIDFILE="${SSH_PIDFILE:-/tmp/dropbear_alt.pid}"
UPTIME_FILE="${UPTIME_FILE:-/proc/uptime}"
MAINTAIN_EVERY=5      # minutes between the housekeeping below

# Is the dropbear we started still there? The pid file names the listener;
# /proc/<pid>/comm confirms the pid hasn't been reused by something else.
dropbear_alive() {
  pid=""; comm=""
  [ -r "$SSH_PIDFILE" ] && read pid < "$SSH_PIDFILE"
  [ -n "$pid" ] && [ -r "/proc/$pid/comm" ] && read comm < "/proc/$pid/comm"
  [ "$comm" = dropbear ]
}

# True one run in MAINTAIN_EVERY, going by uptime so no extra process is needed.
maintenance_minute() {
  read up _ < "$UPTIME_FILE" || return 1
  up="${up%.*}"
  [ $(( up / 60 % MAINTAIN_EVERY )) -eq 0 ]
}

keep_ssh() {
  [ "$KEEP_SSH" = 1 ] && [ -x "$DROPBEAR" ] || return 0
  started=0
  if ! dropbear_alive; then
    log "dropbear is not running - starting it on port $SSH_PORT"
    (cd "${DROPBEAR%/*}" && "$DROPBEAR" -E -R -p"$SSH_PORT" -P "$SSH_PIDFILE") >/dev/null 2>&1
    started=1
  fi
  # Every few minutes (and straight after starting dropbear): let the port
  # through the firewall, which the Kindle's own scripts can close again when
  # wifi reconnects, and keep the screensaver off, which powerd forgets.
  if [ "$started" = 1 ] || maintenance_minute; then
    iptables -P INPUT ACCEPT >/dev/null 2>&1
    lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1
  fi
}

# The log lives on the small user partition and must never fill it. The app
# caps what it writes itself (-log-max); this is the backstop for everything
# else that lands there. The app holds the file open for appending, so
# emptying it in place is safe and it carries on writing at the start.
LOG_MAX=524288
trim_log() {
  [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt "$LOG_MAX" ] || return 0
  cp "$LOG" "$LOG.1" && : > "$LOG"
}

# Before the pause and disable switches on purpose: those are about the app
# and the screen, and ssh is how you would get in to use them.
keep_ssh
maintenance_minute && trim_log

# --- pause switch -------------------------------------------------------------
# While this file exists the launcher does nothing: it doesn't start the app
# and doesn't restore the stock UI either. For experiments and maintenance,
# where `disable` (which hands the screen back) is too much and an unattended
# restart is too little.
[ -f "$DIR/pause" ] && exit 0

# --- off switch ---------------------------------------------------------------
if [ -f "$DISABLE" ]; then
  if pidof paperdisplay >/dev/null; then
    log "disable file present - stopping the app"
    kill $(pidof paperdisplay)
  fi
  restore_ui
  exit 0
fi

# Already running: nothing to do, and nothing slower than pidof has run to
# get here - this is the path taken every minute. (pidof, not a pidfile, so a
# copy someone started by hand in a terminal counts too and we never run two.)
pidof paperdisplay >/dev/null && exit 0

if [ -z "$SIGNALK_HOST" ]; then
  log "SIGNALK_HOST is not set in $CONF - not starting (copy launcher.conf.example to launcher.conf and edit it)"
  exit 0
fi
if [ ! -x "$BIN" ]; then
  log "$BIN is missing or not executable - not starting"
  exit 0
fi

# --- crash accounting ---------------------------------------------------------
# The wrapper below records the app's exit status; a non-zero one is a crash.
# Zero is a clean exit, which includes the updater installing a new version.
if [ -f "$STATE/exit" ]; then
  code="$(cat "$STATE/exit")"
  rm -f "$STATE/exit"
  if [ "$code" != "0" ]; then
    date +%s >> "$STATE/crashes"
    log "the app exited with status $code"
  fi
fi

if [ -f "$STATE/crashes" ]; then
  now="$(date +%s)"
  : > "$STATE/crashes.new"
  while read -r t; do
    [ $((now - t)) -lt "$WINDOW" ] && echo "$t" >> "$STATE/crashes.new"
  done < "$STATE/crashes"
  mv "$STATE/crashes.new" "$STATE/crashes"
  n="$(wc -l < "$STATE/crashes")"

  if [ "$n" -ge "$ROLLBACK_AFTER" ] && [ -f "$BIN.prev" ]; then
    log "$n crashes in ${WINDOW}s - rolling back to the previous version"
    mv "$BIN" "$BIN.bad"
    mv "$BIN.prev" "$BIN"
    rm -f "$STATE/crashes"
    n=0
  fi
  if [ "$n" -ge "$GIVE_UP_AFTER" ]; then
    log "$n crashes in ${WINDOW}s - restoring the stock Kindle UI; will try again once the crashes age out"
    restore_ui
    exit 0
  fi
fi

# --- start --------------------------------------------------------------------
# Keep the log from growing forever on the (small) user partition.
if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 524288 ]; then
  mv "$LOG" "$LOG.1"
fi

take_screen
log "starting paperdisplay for $SIGNALK_HOST"
cd "$DIR" || exit 1
(
  "$BIN" -signalk "$SIGNALK_HOST" -display fbink -touch -update-every 6h $EXTRA_ARGS >> "$LOG" 2>&1
  echo $? > "$STATE/exit"
) &
exit 0
