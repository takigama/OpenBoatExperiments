#!/bin/sh
# Installs SignalKPaperDisplay on a rooted Kindle, start to finish, pulling
# everything it needs from GitHub. Run it ON the Kindle (an ssh session or
# KOReader's terminal; it needs curl, which the Kindle has):
#
#   curl -sL https://raw.githubusercontent.com/takigama/OpenBoatExperiments/master/SignalKPaperDisplay/install.sh | sh -s -- --signalk 192.168.1.20:3000
#
# or from a PC, which reaches the Kindle over ssh (the Kindle still downloads
# everything itself, so it needs internet):
#
#   sh install.sh --host 192.168.1.50 --signalk 192.168.1.20:3000
#   curl -sL <the address above> | sh -s -- --host 192.168.1.50 --signalk 192.168.1.20:3000
#
# It works out which Kindle this is and whether there is a profile for it. If
# there isn't, it stops, changes nothing, and tells you to run the collector
# (tools/kindle-probe.sh) and paste its report. If there is, it downloads the
# app (checked against its published sha256) and FBInk, writes launcher.conf,
# adds the launcher to the crontab, starts it, and leaves uninstall.sh beside
# the app. Every step is safe to repeat, so run it again to update.
#
# Options:
#   --check               only report: which Kindle, which profile, what would be
#                         installed. Changes nothing.
#   --signalk HOST:PORT   your SignalK server, written to launcher.conf (and
#                         changed there if it exists). Without it, and with no
#                         launcher.conf yet, the files are installed but the app
#                         is not started.
#   --uninstall           take it off again (add --keep-ssh or --purge)
#   --platform NAME       use this profile instead of the detected one
#   --ref REF             a git tag, branch or commit to install from (default
#                         master); pins the app version too
#   --remove-startssh     also remove an old hand-made startssh.sh cron job
#   --no-cron             do not touch the Kindle's crontab
#   --no-start            do not start the app at the end
#   --dir PATH            install here instead of /mnt/us/signalk
#   --host H [--port P] [--user U]
#                         run all of this on the Kindle at H over ssh (default
#                         port 2223, user root)
#   -h, --help            this text
#
# Only files this project owns are written, under /mnt/us/signalk, plus one
# line in the crontab. Plain POSIX sh: the Kindle has busybox ash. The whole
# script sits inside main(), called on the last line, so a download cut short
# runs nothing.

REPO="takigama/OpenBoatExperiments"
DEFAULT_REF="master"

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: curl -sL <install.sh address> | sh -s -- [options]
   or: sh install.sh [options]

  --check               only report: which Kindle, which profile, what would be installed
  --signalk HOST:PORT   your SignalK server (written to launcher.conf)
  --uninstall           take it off again (with --keep-ssh or --purge)
  --platform NAME       use this profile instead of the detected one
  --ref REF             a git tag, branch or commit to install from (default master)
  --remove-startssh     also remove an old hand-made startssh.sh cron job
  --no-cron             do not touch the Kindle's crontab
  --no-start            do not start the app at the end
  --dir PATH            install here instead of /mnt/us/signalk
  --host H [--port P] [--user U]   run it on the Kindle at H over ssh
  -h, --help            this text
EOF
}

# Values that end up in a remote command line or a sed script: only plain characters.
valid() { printf '%s' "$1" | grep -Eq '^[A-Za-z0-9._:/-]+$'; }

# A cache-busting query for GitHub's raw host (it caches for minutes); not for
# file:// addresses, which the tests use.
bust() { case "$1" in https:*) printf '%s?t=%s' "$1" "$(date +%s)" ;; *) printf '%s' "$1" ;; esac; }

fetch() { # url dest
  curl -fsSL --connect-timeout 15 --max-time 600 "$(bust "$1")" -o "$2" </dev/null
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | sed 's/.*= *//'
  else
    die "no sha256sum or openssl here, so downloads cannot be checked"
  fi
}

# --- what is this Kindle? -----------------------------------------------------

find_fbink() {
  for c in "${FBINK_BIN:-}" "$DIR/fbink" /mnt/us/koreader/fbink "$(command -v fbink 2>/dev/null)"; do
    [ -n "$c" ] && [ -x "$c" ] && { printf '%s' "$c"; return; }
  done
}

detect() {
  CODENAME=""; MODEL=""; W=""; H=""; SERIAL4=""
  fb="$(find_fbink)"
  if [ -n "$fb" ]; then
    e="$("$fb" -e 2>/dev/null </dev/null | tr ';' '\n')"
    CODENAME="$(printf '%s\n' "$e" | sed -n "s/^deviceCodename='\(.*\)'\$/\1/p" | head -1)"
    MODEL="$(printf '%s\n' "$e" | sed -n "s/^deviceName='\(.*\)'\$/\1/p" | head -1)"
    W="$(printf '%s\n' "$e" | sed -n 's/^viewWidth=\([0-9]*\)$/\1/p' | head -1)"
    H="$(printf '%s\n' "$e" | sed -n 's/^viewHeight=\([0-9]*\)$/\1/p' | head -1)"
  fi
  if [ -z "$W" ]; then   # no FBInk: the first mode the framebuffer lists
    m="$(sed -n 's/^[A-Z]*:\([0-9]*\)x\([0-9]*\).*/\1 \2/p' /sys/class/graphics/fb0/modes 2>/dev/null | head -1)"
    W="${m% *}"; H="${m#* }"
  fi
  # Only the first four characters of the serial: it names the model, not the unit.
  SERIAL4="$(head -c 4 "${INSTALL_USID:-/proc/usid}" 2>/dev/null | tr -cd 'A-Za-z0-9')"
  FIRMWARE="$(head -1 /etc/prettyversion.txt 2>/dev/null | sed 's/ *(.*//')"
}

# devices.txt: "<platform> key=value ..." per line; a Kindle matches the first
# line whose every key matches what was detected.
match_platform() { # file
  MATCHED=""
  size="${W}x${H}"
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    set -f; set -- $line; set +f
    p="$1"; shift
    ok=1
    [ $# -gt 0 ] || ok=0
    for kv in "$@"; do
      k="${kv%%=*}"; v="${kv#*=}"
      case "$k" in
        codename) [ "$CODENAME" = "$v" ] || ok=0 ;;
        serial)   [ "$SERIAL4" = "$v" ] || ok=0 ;;
        size)     [ "$size" = "$v" ] || ok=0 ;;
        *)        ok=0 ;;
      esac
    done
    if [ "$ok" = 1 ]; then MATCHED="$p"; return; fi
  done < "$1"
}

# --- the installer proper (runs on the Kindle) --------------------------------

run_here() {
  command -v curl >/dev/null 2>&1 || die "curl is missing, and this needs it"
  if [ -z "${KINDLE_DIR:-}" ]; then
    [ -d /mnt/us ] || die "this does not look like a Kindle (there is no /mnt/us)"
    case "$(uname -m)" in arm*) ;; *) die "this is a $(uname -m) machine; the app is built for the Kindle's ARM" ;; esac
  fi

  BASE="${INSTALL_BASE:-https://raw.githubusercontent.com/$REPO/$REF/SignalKPaperDisplay}"
  T="$(mktemp -d "${TMPDIR:-/tmp}/kindle-install.XXXXXX")" || die "cannot make a temporary directory"
  trap 'rm -rf "$T"' EXIT

  if [ "$UNINSTALL" = 1 ]; then
    # The newest uninstaller if we can get it, else the copy installed with the app.
    if fetch "$BASE/platforms/kindle-pw3/install/device-uninstall.sh" "$T/uninstall.sh" && head -1 "$T/uninstall.sh" | grep -q '^#!/bin/sh'; then
      u="$T/uninstall.sh"
    elif [ -f "$DIR/uninstall.sh" ]; then
      u="$DIR/uninstall.sh"
    else
      die "could not download the uninstaller, and there is none in $DIR"
    fi
    DIR="$DIR" sh "$u" $UNARGS </dev/null
    exit $?
  fi

  detect
  say "== this Kindle =="
  say "model:     ${MODEL:-unknown}${CODENAME:+ (codename $CODENAME)}"
  say "screen:    ${W:-?}x${H:-?}"
  say "serial:    ${SERIAL4:-unknown} (first four characters)"
  say "firmware:  ${FIRMWARE:-unknown}"
  [ -n "$(find_fbink)" ] || say "(no FBInk found to ask, so the model above comes from the screen and serial alone)"

  fetch "$BASE/platforms/devices.txt" "$T/devices.txt" || die "could not download the device list from $BASE"
  match_platform "$T/devices.txt"

  say
  say "== profile =="
  if [ -n "$PLATFORM_ARG" ]; then
    if [ -n "$MATCHED" ] && [ "$MATCHED" != "$PLATFORM_ARG" ]; then
      say "warning: this looks like $MATCHED, but you asked for $PLATFORM_ARG"
    fi
    P="$PLATFORM_ARG"
    say "using $P, as you asked"
  elif [ -n "$MATCHED" ]; then
    P="$MATCHED"
    say "found: $P"
  else
    say "none: there is no profile for this Kindle yet."
    say
    say "Nothing was changed. To get one made, run this and paste everything between"
    say "BEGIN and END where you ask for support:"
    say
    say "  curl -sL https://raw.githubusercontent.com/$REPO/$REF/SignalKPaperDisplay/tools/kindle-probe.sh | sh"
    exit 3
  fi

  fetch "$BASE/platforms/$P/profile.json" "$T/profile.json" || die "there is no profile named $P (no platforms/$P/profile.json)"
  grep -q "\"name\": *\"$P\"" "$T/profile.json" || die "platforms/$P/profile.json does not name itself $P"
  desc="$(sed -n 's/.*"description": *"\([^"]*\)".*/\1/p' "$T/profile.json" | head -1)"
  [ -z "$desc" ] || say "           $desc"

  fetch "$BASE/update/manifest.json" "$T/manifest.json" || die "could not download the release manifest"
  entry="$(grep "\"$P\"" "$T/manifest.json" | head -1)"
  M_VER="$(printf '%s' "$entry" | sed -n 's/.*"version": *\([0-9]*\).*/\1/p')"
  M_URL="$(printf '%s' "$entry" | sed -n 's/.*"url": *"\([^"]*\)".*/\1/p')"
  M_SUM="$(printf '%s' "$entry" | sed -n 's/.*"sha256": *"\([0-9a-f]\{64\}\)".*/\1/p')"
  [ -n "$M_VER" ] && [ -n "$M_URL" ] && [ -n "$M_SUM" ] || die "no released build of $P is listed in the manifest yet"
  case "$M_URL" in https://github.com/*) ;; file://*) [ -n "${INSTALL_BASE:-}" ] || die "refusing a file:// download address" ;; *) die "unexpected download address in the manifest: $M_URL" ;; esac

  say
  say "== what would be installed =="
  say "app:       $P build $M_VER"
  say "directory: $DIR$([ -f "$DIR/paperdisplay" ] && echo ' (already installed there: this updates it)')"
  say "from:      $BASE"

  if [ "$CHECK" = 1 ]; then
    say
    say "That was --check: nothing was changed. Run it again without --check to install."
    exit 0
  fi

  # --- download, into a scratch directory inside the install directory (so the
  # moves into place are renames on one filesystem) ---------------------------
  mkdir -p "$DIR" || die "cannot create $DIR"
  S="$DIR/.install.$$"
  rm -rf "$S"; mkdir "$S" || die "cannot write in $DIR"
  trap 'rm -rf "$T" "$S"' EXIT

  say
  say "== downloading =="
  fetch "$M_URL" "$S/paperdisplay" || die "could not download the app from $M_URL"
  [ "$(sha256_of "$S/paperdisplay")" = "$M_SUM" ] || die "the downloaded app does not match its published sha256: not installing it"
  say "app:       ok, sha256 checked"

  fetch "$BASE/tools/fbink/prebuilt/fbink-kindlepw2" "$S/fbink" || die "could not download FBInk"
  fetch "$BASE/tools/fbink/prebuilt/fbink-kindlepw2.sha256" "$T/fbink.sha256" || die "could not download FBInk's checksum"
  [ "$(sha256_of "$S/fbink")" = "$(cut -d' ' -f1 "$T/fbink.sha256" | head -1)" ] || die "FBInk does not match its published sha256: not installing it"
  say "FBInk:     ok, sha256 checked"

  cp "$T/profile.json" "$S/profile.json"
  for f in startpaper.sh install-cron.sh; do
    fetch "$BASE/platforms/kindle-pw3/install/$f" "$S/$f" || die "could not download $f"
    head -1 "$S/$f" | grep -q '^#!/bin/sh' || die "$f did not download properly"
  done
  fetch "$BASE/platforms/kindle-pw3/install/device-uninstall.sh" "$S/uninstall.sh" || die "could not download the uninstaller"
  # A platform's own example config (the jobs to stop differ between Kindles), else the shared one.
  fetch "$BASE/platforms/$P/install/launcher.conf.example" "$S/launcher.conf.example" 2>/dev/null \
    || fetch "$BASE/platforms/kindle-pw3/install/launcher.conf.example" "$S/launcher.conf.example" \
    || die "could not download launcher.conf.example"
  say "scripts:   startpaper.sh install-cron.sh uninstall.sh launcher.conf.example profile.json"

  chmod +x "$S"/* || die "chmod failed"
  # One rename each: writing over a program that is running (the app, or the
  # fbink it calls every second) fails with "text file busy" or catches it
  # half-written, and a rename never does.
  for f in "$S"/*; do mv -f "$f" "$DIR/" || die "could not move $(basename "$f") into $DIR"; done
  rmdir "$S"
  say "installed into $DIR"

  # --- launcher.conf -----------------------------------------------------------
  say
  say "== configuration =="
  HAVE_CONF=0; [ -f "$DIR/launcher.conf" ] && HAVE_CONF=1
  if [ -z "$SIGNALK" ] && [ "$HAVE_CONF" = 0 ] && ( : < /dev/tty ) 2>/dev/null; then
    printf 'SignalK server (host or host:port, e.g. 192.168.1.20:3000): ' > /dev/tty
    read -r SIGNALK < /dev/tty
    if [ -n "$SIGNALK" ] && ! printf '%s' "$SIGNALK" | grep -Eq '^[A-Za-z0-9._-]+(:[0-9]{1,5})?$'; then
      die "that is not a host or host:port: $SIGNALK"
    fi
  fi
  if [ "$HAVE_CONF" = 1 ]; then
    if [ -n "$SIGNALK" ]; then
      sed -i "s|^SIGNALK_HOST=.*|SIGNALK_HOST=$SIGNALK|" "$DIR/launcher.conf"
      say "launcher.conf: SignalK server set to $SIGNALK (the rest is as you had it)"
    else
      say "launcher.conf already exists: left alone"
    fi
  elif [ -n "$SIGNALK" ]; then
    sed "s|^SIGNALK_HOST=.*|SIGNALK_HOST=$SIGNALK|" "$DIR/launcher.conf.example" > "$DIR/launcher.conf"
    say "launcher.conf written from the example, with SignalK at $SIGNALK"
    HAVE_CONF=1
  else
    say "No SignalK server given, so launcher.conf was not written."
    say "  Run this again with  --signalk HOST:PORT  (or copy launcher.conf.example yourself)."
  fi

  # --- cron ---------------------------------------------------------------------
  say
  say "== start at boot (crontab) =="
  if [ "$DO_CRON" = 1 ]; then
    DIR="$DIR" sh "$DIR/install-cron.sh" add </dev/null || say "the crontab was not changed (see above): the app will not start by itself after a reboot"
  else
    say "skipped (--no-cron): the launcher will not run by itself"
  fi

  # --- start --------------------------------------------------------------------
  say
  say "== starting =="
  if [ "$DO_START" = 1 ] && [ "$HAVE_CONF" = 1 ]; then
    ( cd "$DIR" && DIR="$DIR" sh "$DIR/startpaper.sh" </dev/null )
    sleep "${INSTALL_START_WAIT:-10}"
    say "--- log"
    tail -12 "$DIR/paperdisplay.log" 2>/dev/null
    say "---"
    if pidof paperdisplay >/dev/null 2>&1; then
      say "RUNNING: paperdisplay is up"
    else
      say "NOT RUNNING: see the log above"
    fi
  elif [ "$HAVE_CONF" = 0 ]; then
    say "not started: there is no launcher.conf yet"
  else
    say "not started (--no-start)"
  fi

  # --- the old ssh job ----------------------------------------------------------
  if [ "$DO_CRON" = 1 ]; then
    if [ "$RM_STARTSSH" = 1 ]; then
      say
      say "== the old startssh.sh job =="
      DIR="$DIR" sh "$DIR/install-cron.sh" remove-startssh </dev/null
    elif DIR="$DIR" sh "$DIR/install-cron.sh" status </dev/null 2>/dev/null | grep -q 'startssh.sh cron entry: present'; then
      say
      say "Note: your crontab still has a startssh.sh job. The launcher keeps ssh alive"
      say "now, so it is no longer needed: run this again with --remove-startssh to take it out."
    fi
  fi

  say
  say "Done. To change settings later, tap the cog on the screen. To take it all off again:"
  say "  sh $DIR/uninstall.sh"
}

# --- from a PC: do all of the above on the Kindle over ssh --------------------

run_remote() {
  valid "$HOST" && valid "$PORT" && valid "$RUSER" || die "--host, --port and --user take only letters, digits and . : / _ -"
  envp=""
  [ -n "${INSTALL_BASE:-}" ] && envp="INSTALL_BASE='$INSTALL_BASE' "     # for the tests
  ssh_cmd="ssh -p $PORT -o StrictHostKeyChecking=accept-new ${INSTALL_SSH_OPTS:-}"
  case "$0" in
    */install.sh|install.sh)
      if [ -f "$0" ]; then
        # A checkout: send this very script, so local changes are what runs.
        $ssh_cmd "$RUSER@$HOST" "${envp}sh -s --$FWD" < "$0"
        exit $?
      fi ;;
  esac
  $ssh_cmd "$RUSER@$HOST" "curl -fsSL 'https://raw.githubusercontent.com/$REPO/$REF/SignalKPaperDisplay/install.sh' | ${envp}sh -s --$FWD"
  exit $?
}

main() {
  HOST=""; PORT="2223"; RUSER="root"
  SIGNALK=""; PLATFORM_ARG=""; REF="$DEFAULT_REF"; DIR="${KINDLE_DIR:-/mnt/us/signalk}"
  CHECK=0; UNINSTALL=0; DO_CRON=1; DO_START=1; RM_STARTSSH=0; UNARGS=""; FWD=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --host)            HOST="${2:?--host wants a name or address}"; shift 2 ;;
      --port)            PORT="${2:?--port wants a number}"; shift 2 ;;
      --user)            RUSER="${2:?--user wants a name}"; shift 2 ;;
      --signalk)         SIGNALK="${2:?--signalk wants HOST:PORT}"; FWD="$FWD --signalk $2"; shift 2 ;;
      --platform)        PLATFORM_ARG="${2:?--platform wants a name}"; FWD="$FWD --platform $2"; shift 2 ;;
      --ref)             REF="${2:?--ref wants a git tag, branch or commit}"; FWD="$FWD --ref $2"; shift 2 ;;
      --dir)             DIR="${2:?--dir wants a path}"; FWD="$FWD --dir $2"; shift 2 ;;
      --check)           CHECK=1; FWD="$FWD --check"; shift ;;
      --uninstall)       UNINSTALL=1; FWD="$FWD --uninstall"; shift ;;
      --keep-ssh|--purge) UNARGS="$UNARGS $1"; FWD="$FWD $1"; shift ;;
      --remove-startssh) RM_STARTSSH=1; FWD="$FWD --remove-startssh"; shift ;;
      --no-cron)         DO_CRON=0; FWD="$FWD --no-cron"; shift ;;
      --no-start)        DO_START=0; FWD="$FWD --no-start"; shift ;;
      -h|--help)         usage; exit 0 ;;
      *)                 printf 'unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
  done

  for v in "$SIGNALK" "$PLATFORM_ARG" "$REF" "$DIR"; do
    [ -z "$v" ] || valid "$v" || die "an option value has characters it should not: $v"
  done
  if [ -n "$SIGNALK" ] && ! printf '%s' "$SIGNALK" | grep -Eq '^[A-Za-z0-9._-]+(:[0-9]{1,5})?$'; then
    die "--signalk wants HOST or HOST:PORT (letters, digits, dots and dashes), got: $SIGNALK"
  fi
  if [ -n "$UNARGS" ] && [ "$UNINSTALL" = 0 ]; then die "--keep-ssh and --purge go with --uninstall"; fi
  case "$UNARGS" in *--keep-ssh*--purge*|*--purge*--keep-ssh*) die "--purge deletes the launcher, so it cannot be combined with --keep-ssh" ;; esac

  [ -n "$HOST" ] && run_remote
  run_here
}

main "$@"
