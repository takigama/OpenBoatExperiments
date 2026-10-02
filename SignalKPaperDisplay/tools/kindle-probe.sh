#!/bin/sh
# Collects what is needed to support a Kindle that has been rooted: the screen,
# the touch panel, whether it has a front light, the battery, the stock UI jobs
# to stop, the CPU, and whether the app's tools will run. It READS only; it
# changes nothing on the device. It ends with a suggested profile.json for the
# new platform, so the report is everything a new platform directory needs.
#
# On the Kindle (over ssh, or from a terminal such as KTerm), fetch and run it
# in one go, then copy the text it prints:
#
#   curl -sL https://raw.githubusercontent.com/takigama/OpenBoatExperiments/master/SignalKPaperDisplay/tools/kindle-probe.sh | sh
#
# or, to keep the output in a file you can read off the Kindle's USB drive:
#
#   curl -sL <the same address> | sh > /mnt/us/kindle-report.txt
#
# Privacy: it prints the first four characters of the serial number (they name
# the model) and nothing else that identifies you or your network - no MAC
# address, no IP address, no WiFi names, no accounts. Read it before sharing it.

# Find a tool the app can use to read the screen: ours, KOReader's, or the system's.
find_fbink() {
  for f in /mnt/us/signalk/fbink /mnt/us/koreader/fbink /usr/bin/fbink; do
    [ -x "$f" ] && { echo "$f"; return 0; }
  done
  return 1
}

# abs_has "<hex words from an ABS= line>" <bits per word> <bit number>
# The kernel prints a capability bitmap as hex words, most significant first;
# is bit N set? Used to tell a type-B multitouch panel (it has ABS_MT_SLOT,
# bit 47) from other kinds.
abs_has() {
  words="$1"; bits="$2"; n="$3"
  total=0; for w in $words; do total=$((total + 1)); done
  want=$((n / bits)); idx=0
  for w in $words; do
    if [ $((total - 1 - idx)) = "$want" ]; then
      v=$((0x$w))
      [ $(((v >> (n % bits)) & 1)) = 1 ] && return 0
      return 1
    fi
    idx=$((idx + 1))
  done
  return 1
}

# touch_protocol "<ABS words>" <bits per word>
touch_protocol() {
  if abs_has "$1" "$2" 53 && abs_has "$1" "$2" 54; then   # ABS_MT_POSITION_X and _Y
    if abs_has "$1" "$2" 47; then echo "mt-slot"; else echo "mt-type-a"; fi
  elif abs_has "$1" "$2" 0 && abs_has "$1" "$2" 1; then   # ABS_X and ABS_Y
    echo "single-touch"
  else
    echo "none"
  fi
}

# Only the functions above are wanted when the tests source this file.
[ -n "${PROBE_LIB:-}" ] && return 0

section() { echo; echo "== $1 =="; }

BITS=32
case "$(uname -m 2>/dev/null)" in aarch64|arm64|x86_64) BITS=64 ;; esac

echo "===== BEGIN KINDLE REPORT (copy everything down to END) ====="
echo "kindle-probe 1 - read-only"

section "software"
cat /etc/prettyversion.txt 2>/dev/null
uname -a 2>/dev/null
echo "model code (first 4 characters of the serial): $(head -c 4 /proc/usid 2>/dev/null)"
FBINK="$(find_fbink)" && FBV="$("$FBINK" -V 2>&1 | head -3)"
[ -n "${FBV:-}" ] && echo "$FBV"

section "cpu"
grep -iE "model name|hardware|CPU architecture|Features|CPU implementer|CPU part" /proc/cpuinfo 2>/dev/null | head -8
echo "uname -m: $(uname -m 2>/dev/null)"
ARCH="$(uname -m 2>/dev/null)"
FEATS="$(grep -i '^Features' /proc/cpuinfo 2>/dev/null | head -1)"
CPUARCH="$(sed -n 's/^CPU architecture *: *//p' /proc/cpuinfo 2>/dev/null | head -1)"
head -2 /proc/meminfo 2>/dev/null

section "screen"
for f in virtual_size bits_per_pixel stride rotate modes name; do
  v="$(cat /sys/class/graphics/fb0/$f 2>/dev/null | tr '\n' ' ')"
  [ -n "$v" ] && echo "fb0/$f: $v"
done
VIEW_W=""; VIEW_H=""; DPI=""
if [ -n "$FBINK" ]; then
  EOUT="$("$FBINK" -e 2>&1 | head -1)"
  echo "fbink -e: $EOUT"
  VIEW_W="$(echo "$EOUT" | sed -n 's/.*viewWidth=\([0-9]*\);.*/\1/p')"
  VIEW_H="$(echo "$EOUT" | sed -n 's/.*viewHeight=\([0-9]*\);.*/\1/p')"
  DPI="$(echo "$EOUT" | sed -n 's/.*DPI=\([0-9]*\);.*/\1/p')"
fi
# Without FBInk, the first mode the framebuffer lists (WxH).
if [ -z "$VIEW_W" ]; then
  m="$(sed -n 's/^[A-Z]*:\([0-9]*\)x\([0-9]*\).*/\1 \2/p' /sys/class/graphics/fb0/modes 2>/dev/null | head -1)"
  VIEW_W="${m% *}"; VIEW_H="${m#* }"
fi

section "touch"
TOUCH_DEV=""; TOUCH_PROTO=""; TOUCH_NAME=""
name=""; handlers=""; abs=""
report_block() {
  [ -z "$abs" ] && return
  ev="$(echo "$handlers" | tr ' ' '\n' | grep -m1 '^event')"
  proto="$(touch_protocol "$abs" "$BITS")"
  echo "input: $name  handlers: $handlers  protocol: $proto"
  if [ "$proto" != none ] && [ -z "$TOUCH_DEV" ] && [ -n "$ev" ]; then
    TOUCH_DEV="/dev/input/$ev"; TOUCH_PROTO="$proto"; TOUCH_NAME="$name"
  fi
}
while IFS= read -r line; do
  case "$line" in
    "N: Name="*)     name="${line#N: Name=}" ;;
    "H: Handlers="*) handlers="${line#H: Handlers=}" ;;
    "B: ABS="*)      abs="${line#B: ABS=}" ;;
    "")              report_block; name=""; handlers=""; abs="" ;;
  esac
done < /proc/bus/input/devices
report_block
ls /dev/input 2>/dev/null | tr '\n' ' '; echo

section "front light"
LIGHT=""
for d in /sys/class/backlight/*; do
  [ -d "$d" ] || continue
  echo "backlight: $d  brightness=$(cat $d/brightness 2>/dev/null) max=$(cat $d/max_brightness 2>/dev/null)"
  LIGHT="sysfs"
done
FLI="$(lipc-get-prop com.lab126.powerd flIntensity 2>/dev/null | head -1)"
FLMAX="$(lipc-get-prop com.lab126.powerd flMaxIntensity 2>/dev/null | head -1)"
echo "lipc flIntensity: ${FLI:-(none)}   flMaxIntensity: ${FLMAX:-(none)}"
if [ -z "$LIGHT" ]; then
  case "$FLMAX" in ''|*[!0-9]*) ;; *) [ "$FLMAX" -gt 0 ] && LIGHT="lipc" ;; esac
fi
echo "verdict: ${LIGHT:-no front light found}"

section "battery and charger"
for d in /sys/class/power_supply/*; do
  [ -d "$d" ] || continue
  printf '%s:' "$d"
  for f in type status online charging present capacity; do
    [ -r "$d/$f" ] && printf ' %s=%s' "$f" "$(cat $d/$f)"
  done
  echo
done
lipc-get-prop com.lab126.powerd battLevel 2>/dev/null | sed 's/^/lipc battLevel: /'

section "stock ui"
JOBS="$(initctl list 2>/dev/null | grep -iE 'framework|pillow|statusbar|status_bar|lab126_gui|home|webreader|blanket|awesome|juno' )"
echo "$JOBS"
echo "-- running"
(ps -ef 2>/dev/null || ps) | grep -iE 'cvm|framework|pillowd|statusbar|StatusBar|awesome|blanket|powerd|volumd' | grep -v grep | cut -c1-120

section "cron and ssh"
CRON_DIR="$( (ps -ef 2>/dev/null || ps) | grep '[c]rond' | sed -n 's/.* -c \([^ ]*\).*/\1/p' | head -1)"
echo "crond directory: ${CRON_DIR:-(crond not found)}"
[ -f "${CRON_DIR:-/etc/crontab}/root" ] && grep -vE '^#' "${CRON_DIR:-/etc/crontab}/root" | cut -c1-120
echo "-- dropbear"
for f in /mnt/us/koreader/dropbear /usr/sbin/dropbear; do
  [ -x "$f" ] && { echo "found: $f"; "$f" -V 2>&1 | head -1; }
done
(ps -ef 2>/dev/null || ps) | grep '[d]ropbear' | cut -c1-140 | head -3
ls /mnt/us/koreader/settings/SSH 2>/dev/null | sed 's/^/koreader ssh folder: /'
mount 2>/dev/null | grep -E ' on / ' | tail -1

section "tools and network"
for t in curl wget lua nc busybox sh awk sed tar md5sum mntroot lipc-get-prop lipc-set-prop initctl; do
  p="$(command -v $t 2>/dev/null)"; echo "$t: ${p:-no}"
done
curl --version 2>/dev/null | head -1
# Can it reach GitHub (the updater needs this)? A status code, nothing else.
code="$(curl -s -o /dev/null -m 8 -w '%{http_code}' https://raw.githubusercontent.com/ 2>/dev/null)"
echo "https to raw.githubusercontent.com: ${code:-no answer}"

section "storage"
df -h /mnt/us / 2>/dev/null
ls /mnt/us 2>/dev/null | head -20 | tr '\n' ' '; echo

# --- what to do about it -------------------------------------------------------
section "suggested platform"
case "$ARCH:$CPUARCH" in
  aarch64:*|arm64:*) GOARCH=arm64; GOARM="" ;;
  arm*:7|armv7*:*)   GOARCH=arm;   GOARM=7 ;;
  arm*:6|armv6*:*)   GOARCH=arm;   GOARM=6 ;;
  *)                 GOARCH=arm;   GOARM=5 ;;
esac
echo "build.env:"
echo "  GOOS=linux"; echo "  GOARCH=$GOARCH"; [ -n "$GOARM" ] && echo "  GOARM=$GOARM"
case "$GOARM" in 5|6) echo "  (older than the Paperwhite 3's ARMv7: untested - needs a legacy FBInk and a check for hardware floating point: $FEATS)" ;; esac

MODEL="$(echo "$FBV" | sed -n 's/.*Detected a \(.*\) (.*/\1/p' | head -1)"
SLUG="$(echo "${MODEL:-$(head -c 4 /proc/usid 2>/dev/null)}" | tr 'A-Z ' 'a-z-' | tr -cd 'a-z0-9-')"
echo
echo "profile.json:"
echo "{"
echo "  \"name\": \"kindle-${SLUG:-new}\","
echo "  \"description\": \"${MODEL:-Kindle}, ${VIEW_W:-?}x${VIEW_H:-?}, $(sed -n 's/^Kindle \([0-9.]*\).*/firmware \1/p' /etc/prettyversion.txt 2>/dev/null | head -1)\","
echo "  \"width\": ${VIEW_W:-0},"
echo "  \"height\": ${VIEW_H:-0},"
echo "  \"grayLevels\": 16,"
echo "  \"rotation\": 0,"
echo "  \"fetch\": \"curl\","
if [ "$LIGHT" = sysfs ]; then
  echo "  \"frontlight\": { \"sysfs\": \"/sys/class/backlight/*\" },"
elif [ "$LIGHT" = lipc ]; then
  echo "  \"frontlight\": { \"lipc\": \"com.lab126.powerd flIntensity\", \"lipcMax\": $FLMAX },"
fi
echo "  \"touch\": {"
echo "    \"device\": \"${TOUCH_DEV:-/dev/input/event?}\","
echo "    \"protocol\": \"${TOUCH_PROTO:-unknown}\""
echo "  }"
echo "}"
[ "${TOUCH_PROTO:-}" = mt-slot ] || echo "  (the app only reads type-B multitouch panels so far: this one needs new touch code)"
[ -z "$LIGHT" ] && echo "  (no front light found, so there is no frontlight section: the Backlight setting will be hidden)"

STOP=""
echo "$JOBS" | grep -q '^framework ' && STOP="framework"
echo "$JOBS" | grep -qE '^(statusbar|status_bar) ' && STOP="$STOP $(echo "$JOBS" | grep -oE '^(statusbar|status_bar)' | head -1)"
echo
echo "launcher.conf.example:"
echo "  STOP_JOBS=\"${STOP:-framework}\""
echo "  (the jobs that draw the stock UI and its status bar; confirm the clock stays off the screen)"

echo
echo "== done =="
echo "===== END KINDLE REPORT ====="
echo "Paste everything from BEGIN to END when asking for support for this Kindle."
