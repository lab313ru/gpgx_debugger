#!/usr/bin/env bash
# Prove a Linux package of the standalone app runs on its own, away from the
# build tree:
#
#   tools/check_linux_package.sh <unpacked standalone dir | AppImage> <rom> [build dir]
#
# Starts the app on the current display with an empty LD_LIBRARY_PATH and
# DGX_NO_AUDIO=1, waits for its control socket and asks it ping, status and
# frame. Then it checks where the libraries come from, two ways:
#
#   ldd over every ELF file of the package as the app runs it, and
#   /proc/<pid>/maps of the running app: what was actually loaded.
#
# Any library ldd cannot find, any Qt library from outside the package, or
# anything at all from the build directory (default <repo>/builds/linux-release)
# fails the check. An AppImage runs with APPIMAGE_EXTRACT_AND_RUN=1, so FUSE is
# not needed, and is checked in the directory it unpacked itself into.
set -euo pipefail

die() { printf 'check_linux_package: %s\n' "$*" >&2; exit 1; }
[ $# -ge 2 ] || die "usage: $0 <unpacked dir | AppImage> <rom> [build dir]"
PKG=$(realpath "$1")
ROM=$(realpath "$2")
BUILD=$(realpath -m "${3:-$(dirname "${BASH_SOURCE[0]}")/../builds/linux-release}")
[ -f "$ROM" ] || die "no ROM at $ROM"
[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || die "no display: set DISPLAY"
command -v python3 >/dev/null || die "needs python3"

if [ -d "$PKG" ]; then
    [ -x "$PKG/AppRun" ] || die "$PKG has no AppRun"
    launch=("$PKG/AppRun" "$ROM")
else
    launch=(env APPIMAGE_EXTRACT_AND_RUN=1 "$PKG" "$ROM")
fi

export LD_LIBRARY_PATH= DGX_NO_AUDIO=1
unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH
LOG=$(mktemp)
APP=
fail=0

"${launch[@]}" > "$LOG" 2>&1 &
LAUNCHER=$!
running() {     # running PID: alive and not a zombie waiting to be reaped
    local s
    s=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    s=${s##*) }
    [ "${s%% *}" != Z ]
}
stop() {
    # The app first: an AppImage runtime that sees it exit removes its /tmp copy.
    kill ${APP:-$LAUNCHER} 2>/dev/null || true
    for _ in $(seq 50); do running "$LAUNCHER" || break; sleep 0.1; done
    for p in $APP $LAUNCHER; do running "$p" && kill "$p" 2>/dev/null && sleep 0.5 || true; done
    for p in $APP $LAUNCHER; do kill -9 "$p" 2>/dev/null || true; done
    wait "$LAUNCHER" 2>/dev/null || true
    if [ "$fail" != 0 ]; then echo "--- app output:"; head -40 "$LOG"; fi
    rm -f "$LOG"
}
trap stop EXIT

echo "== start: ${launch[*]}"
echo "   LD_LIBRARY_PATH='$LD_LIBRARY_PATH' DISPLAY=${DISPLAY:-} build dir: $BUILD"

# The app may be a grandchild (AppImage runtime -> AppRun script -> binary), so
# its bridge is the one whose ping names a pid descended from the launcher.
probe=$(python3 - "$LAUNCHER" <<'PY'
import base64, socket, sys, time

launcher = int(sys.argv[1])

def descends(pid):
    while pid > 1:
        if pid == launcher:
            return True
        try:
            pid = int(open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()[1])
        except OSError:
            return False
    return False

def ask(f, s, line):
    s.sendall((line + "\n").encode())
    return f.readline().rstrip("\r\n")

deadline = time.time() + 40
while time.time() < deadline:
    for port in range(27042, 27042 + 16):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.3)
        except OSError:
            continue
        f = s.makefile("r", encoding="latin-1")
        ping = ask(f, s, "ping")
        fields = dict(kv.split("=", 1) for kv in ping.split()[2:] if "=" in kv)
        pid = int(fields.get("pid", "0"))
        if not (ping.startswith("ok smd_dgx") and descends(pid)):
            s.close()
            continue
        print("APP_PID=%d" % pid)
        print("   ping   -> " + ping)
        # A game opens on black screens, so ask until a frame shows something.
        until = time.time() + 20
        while True:
            time.sleep(1.0)
            frame = ask(f, s, "frame").split(" ", 3)
            if frame[0] != "ok":
                print("FAIL frame -> " + " ".join(frame)[:80])
                sys.exit(1)
            w, h, raw = int(frame[1]), int(frame[2]), base64.b64decode(frame[3])
            lit = sum(1 for i in range(0, len(raw), 2) if raw[i] or raw[i + 1])
            if lit or time.time() > until:
                break
        print("   status -> " + ask(f, s, "status"))
        print("   frame  -> ok %dx%d, %d of %d pixels not black" % (w, h, lit, w * h))
        sys.exit(0 if lit else 1)
    time.sleep(0.25)
print("FAIL no control socket answered for a child of pid %d" % launcher)
sys.exit(1)
PY
) || fail=1
printf '%s\n' "$probe" | grep -v '^APP_PID='
APP=$(printf '%s\n' "$probe" | sed -n 's/^APP_PID=//p')
[ -n "$APP" ] || { fail=1; die "the app never answered"; }

# Where the package actually runs from: the unpacked dir, or the AppImage's
# extraction directory. The binary is <root>/usr/bin/smd_dgx_qt6.
EXE=$(readlink "/proc/$APP/exe")
ROOT=$(dirname "$(dirname "$(dirname "$EXE")")")
echo "== running: $EXE"

echo "== ldd over every ELF file under $ROOT"
n=0
while IFS= read -r -d '' f; do
    [ "$(od -An -N4 -tx1 "$f" | tr -d ' \n')" = 7f454c46 ] || continue
    n=$((n + 1))
    out=$(ldd "$f" 2>&1 || true)
    bad=$(printf '%s\n' "$out" | awk -v root="$ROOT/" -v build="$BUILD/" '
        /not found/                                  { print "  missing: " $0; next }
        $3 ~ /^\// && index($3, build) == 1          { print "  from the build tree: " $0; next }
        $1 ~ /^libQt6/ && index($3, root) != 1       { print "  Qt from outside the package: " $0 }')
    if [ -n "$bad" ]; then fail=1; echo "${f#"$ROOT"/}:"; echo "$bad"; fi
done < <(find "$ROOT" -type f -print0)
echo "   $n ELF files checked"

echo "== loaded by the running app (/proc/$APP/maps)"
maps=$(awk '$6 ~ /\.so/ { print $6 }' "/proc/$APP/maps" | sort -u)
qt_in=$(printf '%s\n' "$maps" | grep '/libQt6' | grep -c "^$ROOT/" || true)
qt_out=$(printf '%s\n' "$maps" | grep '/libQt6' | grep -v "^$ROOT/" || true)
from_build=$(printf '%s\n' "$maps" | grep "^$BUILD/" || true)
echo "   $(printf '%s\n' "$maps" | wc -l) shared objects; Qt from the package: $qt_in"
printf '%s\n' "$maps" | grep -E '/libQt6|/platforms/|/libSDL2' | sed 's/^/     /'
if [ -n "$qt_out" ];      then fail=1; echo "FAIL Qt from outside the package:"; echo "$qt_out"; fi
if [ -n "$from_build" ];  then fail=1; echo "FAIL loaded from the build tree:";  echo "$from_build"; fi
[ "$qt_in" -gt 0 ] || { fail=1; echo "FAIL no Qt library loaded from the package"; }

if [ "$fail" = 0 ]; then echo "== PASS"; else echo "== FAIL"; fi
exit "$fail"
