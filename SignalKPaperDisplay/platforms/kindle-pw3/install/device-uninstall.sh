#!/bin/sh
# Takes the app off this Kindle and gives it back to the stock software. It runs
# ON the Kindle (the installer leaves a copy as /mnt/us/signalk/uninstall.sh):
#
#   sh /mnt/us/signalk/uninstall.sh [options]
#
# or, with nothing installed any more, straight from GitHub:
#
#   curl -sL https://raw.githubusercontent.com/takigama/OpenBoatExperiments/master/SignalKPaperDisplay/install.sh | sh -s -- --uninstall [options]
#
# It gives the stock UI back (stops the app and starts the jobs the launcher had
# stopped), then removes the launcher from the Kindle's crontab. The files stay
# in place, so installing again keeps your settings.
#
# Options:
#   --keep-ssh   leave the launcher in the crontab, switched off with a `disable`
#                file: the stock UI is back, and the launcher goes on doing the
#                one thing the stock software won't, keeping ssh running
#   --purge      also delete everything under /mnt/us/signalk, settings and logs
#                included (not allowed with --keep-ssh, which needs the launcher)
#   -h, --help   this text
#
# The launcher is what keeps ssh running, so without --keep-ssh it stays up only
# until the Kindle next reboots; start it again from KOReader's menu (Network,
# SSH server) when you need it.
#
# DIR overrides the install directory. Plain POSIX sh: the Kindle has busybox ash.

PATH="$PATH:/sbin:/usr/sbin:/bin:/usr/bin"
DIR="${DIR:-/mnt/us/signalk}"

usage() {
  if [ -f "$0" ]; then
    sed -n '2,/^# The launcher is what/p' "$0" | sed 's/^# \{0,1\}//' | sed '$d'
  else
    echo "options: --keep-ssh (leave the launcher in cron, switched off), --purge (delete all files too)"
  fi
}

KEEP_SSH=0; PURGE=0
for a in "$@"; do
  case "$a" in
    --keep-ssh) KEEP_SSH=1 ;;
    --purge)    PURGE=1 ;;
    -h|--help)  usage; exit 0 ;;
    *)          echo "unknown option: $a" >&2; echo >&2; usage >&2; exit 2 ;;
  esac
done
if [ "$KEEP_SSH" = 1 ] && [ "$PURGE" = 1 ]; then
  echo "--purge deletes the launcher, so it cannot be combined with --keep-ssh" >&2
  exit 2
fi

if [ ! -f "$DIR/startpaper.sh" ]; then
  echo "Nothing to remove: $DIR/startpaper.sh is not here."
  exit 0
fi

# 1. Give the stock UI back, the way the launcher itself does: a `disable` file
#    makes its next run stop the app and start the jobs it stopped.
echo "== 1. giving the stock UI back =="
touch "$DIR/disable"
sh "$DIR/startpaper.sh"
sleep "${UNINSTALL_WAIT:-4}"
if pidof paperdisplay >/dev/null; then
  echo "the app is still running (it will stop within a minute: the launcher runs from cron)"
else
  echo "the app has stopped"
fi
initctl status framework 2>/dev/null

# 2. The crontab.
echo
echo "== 2. the launcher =="
if [ "$KEEP_SSH" = 1 ]; then
  echo "kept in the crontab, switched off by $DIR/disable: it now only keeps ssh running."
  echo "To switch the app back on: delete $DIR/disable (or install again)."
else
  if DIR="$DIR" sh "$DIR/install-cron.sh" remove; then
    rm -f "$DIR/disable"
  else
    echo "the crontab could not be changed: the launcher is still in it and $DIR/disable is kept," >&2
    echo "so the app stays off. Fix that and run this again." >&2
    exit 1
  fi
fi

# 3. Files.
echo
echo "== 3. files =="
if [ "$PURGE" = 1 ]; then
  rm -rf "$DIR" /var/tmp/paperdisplay /var/tmp/paperdisplay.png
  echo "deleted $DIR (settings and logs included)"
else
  echo "left in $DIR (settings included): install again puts it all back. --purge deletes them."
fi

echo
echo "Done. The Kindle is running its own software again."
[ "$KEEP_SSH" = 1 ] || echo "ssh stays up until the next reboot; start it from KOReader (Network, SSH server) after that."
exit 0
