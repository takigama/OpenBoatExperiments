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
  lipc-set-prop com.lab126.pillow disableEnablePillow disable >/dev/null 2>&1
  for job in $STOP_JOBS; do
    running "$job" && stop "$job" >/dev/null 2>&1
  done
}

# Re-assert on every run: powerd forgets it across reboots.
lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1

[ -f "$CONF" ] && . "$CONF"
# lab126_gui as well as the framework: with only the framework stopped, the
# Paperwhite 3's stock UI still painted its clock over our header every minute,
# and stopping lab126_gui is what made that stop (pillow, the status bar
# daemon, turned out not to be it).
STOP_JOBS="${STOP_JOBS:-framework lab126_gui}"

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

# Already running: nothing to do. (pidof, not a pidfile, so a copy someone
# started by hand in a terminal counts too and we never run two.)
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
