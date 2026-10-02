#!/bin/sh
# Manages the cron entry that keeps the launcher (startpaper.sh) running. It
# runs ON the Kindle, normally via deploy.sh, and is safe to run again.
#
#   sh install-cron.sh status
#   sh install-cron.sh add [--remove-startssh]
#   sh install-cron.sh remove-startssh
#   sh install-cron.sh remove
#
# The Kindle's root filesystem is read-only, and crond's table lives on it, so
# editing it means `mntroot rw` ... `mntroot ro`. Everything here is arranged
# so that window is as short as it can be and is always closed again: the table
# is backed up first, edited, checked, put back from the backup if the check
# fails, and only then is the filesystem made read-only.
#
# "remove" takes the launcher's entry out again (for uninstalling).
#
# "remove-startssh" (or --remove-startssh) takes out an old hand-made
# startssh.sh cron job. The launcher keeps ssh alive now, so that job only
# costs a few processes a minute - but it is the way you can get in, so it is
# only removed once a live dropbear is confirmed.
#
# Settings, normally left alone (tests override them): DIR (where the app
# lives), CRON_DIR (crond's directory; found from the running crond), MNTROOT,
# SSH_PIDFILE.

DIR="${DIR:-/mnt/us/signalk}"
MNTROOT="${MNTROOT:-mntroot}"
SSH_PIDFILE="${SSH_PIDFILE:-/tmp/dropbear_alt.pid}"
LAUNCHER="$DIR/startpaper.sh"
ENTRY="* * * * * $LAUNCHER > /dev/null 2>&1"
BACKUP="${BACKUP:-$DIR/crontab.root.bak}"

say() { echo "install-cron: $*"; }
fail() { echo "install-cron: $*" >&2; exit 1; }

# crond is started as `crond -f -c <dir>`; the dir holds one file per user.
find_cron_dir() {
  if [ -n "${CRON_DIR:-}" ]; then echo "$CRON_DIR"; return; fi
  d="$( (ps -ef 2>/dev/null || ps) | grep '[c]rond' | sed -n 's/.* -c \([^ ]*\).*/\1/p' | head -n 1)"
  echo "${d:-/etc/crontab}"
}

CRON_FILE="$(find_cron_dir)/root"
[ -f "$CRON_FILE" ] || fail "there is no crontab at $CRON_FILE (is crond running?)"

has_launcher() { grep -qF "$LAUNCHER" "$CRON_FILE"; }
has_startssh() { grep -q 'startssh\.sh' "$CRON_FILE"; }

# Is the dropbear that gets us in really there? Read with builtins only.
dropbear_alive() {
  pid=""; comm=""
  [ -r "$SSH_PIDFILE" ] && read pid < "$SSH_PIDFILE"
  [ -n "$pid" ] && [ -r "/proc/$pid/comm" ] && read comm < "/proc/$pid/comm"
  [ "$comm" = dropbear ]
}

# edit ADD REMOVE_STARTSSH REMOVE_LAUNCHER: change the table (ADD=1 appends the
# launcher entry, the others delete lines) and leave the filesystem read-only
# again.
edit() {
  want_add="$1"; want_remove="$2"; want_unlaunch="${3:-0}"
  cp "$CRON_FILE" "$BACKUP" || fail "could not back up $CRON_FILE to $BACKUP"

  if ! $MNTROOT rw; then
    $MNTROOT ro 2>/dev/null
    fail "could not make the root filesystem writable ($MNTROOT rw failed); nothing was changed"
  fi

  tmp="/var/tmp/cron.root.new.$$"
  rc=0
  cp "$CRON_FILE" "$tmp" || rc=1
  if [ "$rc" = 0 ] && [ "$want_remove" = 1 ]; then
    grep -v 'startssh\.sh' "$tmp" > "$tmp.2"; mv "$tmp.2" "$tmp"
  fi
  if [ "$rc" = 0 ] && [ "$want_unlaunch" = 1 ]; then
    grep -vF -- "$LAUNCHER" "$tmp" > "$tmp.2"; mv "$tmp.2" "$tmp"
  fi
  if [ "$rc" = 0 ] && [ "$want_add" = 1 ]; then
    # A table with no newline at the end would glue our line onto its last one.
    [ -s "$tmp" ] && [ -n "$(tail -c 1 "$tmp")" ] && echo >> "$tmp"
    echo "$ENTRY" >> "$tmp"
  fi
  # Write through the existing file, so its ownership and mode are kept.
  [ "$rc" = 0 ] && { cat "$tmp" > "$CRON_FILE" || rc=1; }
  rm -f "$tmp" "$tmp.2"

  # Check what is there now; if it is wrong, put the backup back.
  if [ "$rc" = 0 ]; then
    [ "$want_add" = 1 ] && ! has_launcher && rc=1
    [ "$want_remove" = 1 ] && has_startssh && rc=1
    [ "$want_unlaunch" = 1 ] && has_launcher && rc=1
  fi
  if [ "$rc" != 0 ]; then
    cat "$BACKUP" > "$CRON_FILE" 2>/dev/null
  fi

  $MNTROOT ro || say "WARNING: could not make the root filesystem read-only again; run: $MNTROOT ro"
  [ "$rc" = 0 ] || fail "editing $CRON_FILE failed; the original was put back (backup: $BACKUP)"
}

cmd="${1:-status}"
shift 2>/dev/null
remove=0
for a in "$@"; do [ "$a" = --remove-startssh ] && remove=1; done

case "$cmd" in
  status)
    has_launcher && say "launcher cron entry: present" || say "launcher cron entry: absent"
    has_startssh && say "startssh.sh cron entry: present" || say "startssh.sh cron entry: absent"
    say "table: $CRON_FILE"
    ;;
  add)
    add=0; rm=0
    has_launcher || add=1
    if [ "$remove" = 1 ] && has_startssh; then
      dropbear_alive || fail "not removing the startssh.sh job: no live dropbear to replace it (run the launcher once first)"
      rm=1
    fi
    if [ "$add" = 0 ] && [ "$rm" = 0 ]; then
      say "nothing to change: the launcher is already in the crontab"
      exit 0
    fi
    edit "$add" "$rm"
    [ "$add" = 1 ] && say "added the launcher to $CRON_FILE (backup: $BACKUP)"
    [ "$rm" = 1 ] && say "removed the old startssh.sh job"
    say "crond picks the change up within a minute"
    ;;
  remove)
    has_launcher || { say "the launcher is not in the crontab"; exit 0; }
    edit 0 0 1
    say "removed the launcher from $CRON_FILE (backup: $BACKUP)"
    ;;
  remove-startssh)
    has_startssh || { say "no startssh.sh job in the crontab"; exit 0; }
    dropbear_alive || fail "not removing the startssh.sh job: no live dropbear to replace it (run the launcher once first)"
    edit 0 1
    say "removed the old startssh.sh job (backup: $BACKUP)"
    ;;
  *)
    fail "usage: sh install-cron.sh status | add [--remove-startssh] | remove | remove-startssh"
    ;;
esac
