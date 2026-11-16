#!/usr/bin/env bash
# Build SMD DGX on Linux and package it for people who will not build it.
#
# Writes to builds/linux-package/ (or --out DIR):
#   smd_dgx-linux-x86_64.AppImage            the standalone app as a single file
#   smd_dgx_standalone_linux_x86_64.tar.gz   the same AppDir unpacked, AppRun and
#                                            all, for machines without FUSE
#   smd_dgx_linux_README.txt                 the README from that tarball, to put
#                                            next to the AppImage
#   smd_dgx_ida_linux_x86_64.tar.gz          the IDA plugin, both loaders, mcp/
#   glibc_floor.txt                          what each binary needs from the host
#
# Run it on Linux from a clone of the repository (it has been run on Ubuntu
# 24.04 under WSL2). It needs cmake, ninja, g++, Qt 6 with qmake6, the SDL2
# headers, curl or wget, and, for the IDA archive, the IDA SDK:
#
#   IDA_SDK_DIR=<ida-sdk>/src tools/package_linux.sh
#
# Without IDA_SDK_DIR the IDA archive is skipped.
#
# linuxdeploy and its Qt plugin are downloaded on first use from their GitHub
# "continuous" releases into a cache outside the repository
# ($SMD_DGX_TOOLS_DIR, default ~/.cache/smd_dgx/tools), and every run says
# where they came from. Both are AppImages; they run with
# APPIMAGE_EXTRACT_AND_RUN=1, so packaging needs no FUSE either.
#
# Options:
#   --out DIR       where the packages go (default: <repo>/builds/linux-package)
#   --skip-tests    do not build and run the test suite first
#
# Environment:
#   IDA_SDK_DIR       the SDK's src/ directory (the one with include/ida.hpp)
#   QMAKE             Qt 6 qmake to deploy from (default: qmake6 on PATH)
#   SMD_DGX_VERSION   the version line in the READMEs (default: git describe)
#   SMD_DGX_TOOLS_DIR the download cache for linuxdeploy
set -euo pipefail

say() { printf '\n== %s\n' "$*"; }
die() { printf 'package_linux: %s\n' "$*" >&2; exit 1; }

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ASSETS=$ROOT/tools/linux
SRC=$ROOT/smd_dgx_cmake
BUILD=$ROOT/builds/linux-release        # the linux-release preset's binaryDir
OUT=$ROOT/builds/linux-package
RUN_TESTS=1

while [ $# -gt 0 ]; do
    case $1 in
        --out)        OUT=${2:?--out needs a directory}; shift 2 ;;
        --out=*)      OUT=${1#--out=}; shift ;;
        --skip-tests) RUN_TESTS=0; shift ;;
        -h|--help)    sed -n '2,/^set -euo/{/^set -euo/d; s/^# \{0,1\}//; p}' "$0"; exit 0 ;;
        *)            die "unknown argument: $1 (see --help)" ;;
    esac
done

# ---- host --------------------------------------------------------------------

[ "$(uname -s)" = Linux ]  || die "run this on Linux"
[ "$(uname -m)" = x86_64 ] || die "x86_64 only: linuxdeploy is fetched as an x86_64 AppImage"
for t in cmake ninja tar gzip objdump readelf strip od dd sha256sum; do
    command -v "$t" >/dev/null || die "missing tool: $t"
done
if [ -z "${QMAKE:-}" ]; then
    QMAKE=$(command -v qmake6 || true)
    [ -n "$QMAKE" ] || [ ! -x /usr/lib/qt6/bin/qmake ] || QMAKE=/usr/lib/qt6/bin/qmake
fi
[ -n "$QMAKE" ] || die "qmake6 not found (Ubuntu: qt6-base-dev-tools); or set QMAKE"
QT_VERSION=$("$QMAKE" -query QT_VERSION)
case $QT_VERSION in 6.*) ;; *) die "$QMAKE is Qt $QT_VERSION, not Qt 6" ;; esac
if [ -n "${IDA_SDK_DIR:-}" ] && [ ! -f "$IDA_SDK_DIR/include/ida.hpp" ]; then
    die "IDA_SDK_DIR=$IDA_SDK_DIR has no include/ida.hpp"
fi
VERSION=${SMD_DGX_VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo unversioned)}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# ---- linuxdeploy ---------------------------------------------------------------

TOOLS=${SMD_DGX_TOOLS_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/smd_dgx/tools}
LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
QTPLUGIN_URL=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

fetch() {   # fetch URL: into $TOOLS unless it is already there
    local url=$1 f=$TOOLS/${1##*/}
    if [ -x "$f" ]; then
        echo "cached:     $f"
        echo "  came from $(cat "$f.source" 2>/dev/null || echo "$url (no record kept)")"
    else
        mkdir -p "$TOOLS"
        echo "downloading $url"
        echo "  into      $f"
        if command -v curl >/dev/null; then
            curl -fL --retry 3 -o "$f.part" "$url"
        elif command -v wget >/dev/null; then
            wget -O "$f.part" "$url"
        else
            die "need curl or wget to download ${url##*/}"
        fi
        chmod +x "$f.part"
        mv "$f.part" "$f"
        echo "$url, downloaded $(date -u +%Y-%m-%dT%H:%MZ)" > "$f.source"
    fi
    echo "  sha256    $(sha256sum "$f" | cut -d' ' -f1)"
}

say "linuxdeploy (cache: $TOOLS)"
fetch "$LINUXDEPLOY_URL"
fetch "$QTPLUGIN_URL"
LINUXDEPLOY=$TOOLS/linuxdeploy-x86_64.AppImage

# The AppImage runtime. Given none, appimagetool downloads one from GitHub. It
# need not: linuxdeploy is an AppImage itself, and its first --appimage-offset
# bytes are exactly that runtime. Its update-information slot names
# linuxdeploy's own releases, though, and an AppImage updater would then
# replace our AppImage with linuxdeploy, so that slot is zeroed.
RUNTIME=$TOOLS/runtime-x86_64
if [ ! -s "$RUNTIME" ] || [ "$RUNTIME" -ot "$LINUXDEPLOY" ]; then
    off=$("$LINUXDEPLOY" --appimage-offset)
    head -c "$off" "$LINUXDEPLOY" > "$RUNTIME.part"
    read -r upd_off upd_size < <(readelf -S -W "$RUNTIME.part" \
        | awk '{ for (i = 1; i < NF; i++) if ($i == ".upd_info") { print $(i+3), $(i+4); exit } }') || true
    [ -n "${upd_size:-}" ] || die "no .upd_info section in the runtime taken from $LINUXDEPLOY"
    dd if=/dev/zero of="$RUNTIME.part" bs=1 seek=$((16#$upd_off)) count=$((16#$upd_size)) \
       conv=notrunc status=none
    mv "$RUNTIME.part" "$RUNTIME"
    echo "first $off bytes of ${LINUXDEPLOY##*/}, .upd_info zeroed" > "$RUNTIME.source"
fi
echo "runtime:    $RUNTIME"
echo "  came from $(cat "$RUNTIME.source")"
echo "  sha256    $(sha256sum "$RUNTIME" | cut -d' ' -f1)"
export LDAI_RUNTIME_FILE=$RUNTIME

export APPIMAGE_EXTRACT_AND_RUN=1      # no FUSE needed to run the tools
export QMAKE

# linuxdeploy looks for its plugins on PATH, so the cache goes first. It also
# dies on the first PATH entry it may not read, and WSL appends the whole
# Windows PATH, some of it unreadable from Linux; nothing here needs Windows
# programs, so those entries go.
path=$TOOLS
IFS=: read -r -a dirs <<< "$PATH"
for d in "${dirs[@]}"; do
    case $d in /mnt/[a-z]/*) continue ;; esac
    if [ -d "$d" ] && [ -r "$d" ] && [ -x "$d" ]; then path+=":$d"; fi
done
export PATH=$path

# ---- build -------------------------------------------------------------------

say "configure: linux-release preset (IDA SDK: ${IDA_SDK_DIR:-none, IDA archive skipped})"
(cd "$SRC" && cmake --preset linux-release)

targets=(smd_dgx_qt6)
[ -z "${IDA_SDK_DIR:-}" ] || targets+=(smd_dgx_ida smd_dgx_ldr smd_dgx_z80_ldr)
[ "$RUN_TESTS" = 0 ]      || targets+=(smd_dgx_tests)
say "build: ${targets[*]}"
cmake --build "$BUILD" --target "${targets[@]}"

if [ "$RUN_TESTS" = 1 ]; then
    say "tests"
    (cd "$BUILD" && DGX_NO_AUDIO=1 ctest --output-on-failure --timeout 300)
fi

# ---- the floor -----------------------------------------------------------------
#
# A binary runs only where the host's glibc and libstdc++ define every symbol
# version it imports. Only *UND* lines count: a library in the bundle also
# *defines* versions, and those ask nothing of the host.

elf_files() {   # elf_files PATH...: every regular ELF file under them
    find "$@" -type f -print0 | while IFS= read -r -d '' f; do
        [ "$(od -An -N4 -tx1 "$f" | tr -d ' \n')" = 7f454c46 ] && printf '%s\n' "$f"
    done
    return 0
}
needs() {       # needs PREFIX FILE...: highest PREFIX_x.y the files import
    local p=$1 f; shift
    [ $# -gt 0 ] || return 0
    for f in "$@"; do objdump -T "$f" 2>/dev/null || true; done \
        | grep -F '*UND*' | grep -oE "\\b${p}_[0-9]+(\\.[0-9]+)*" \
        | sed "s/^${p}_//" | sort -uV | tail -n 1 || true
}
host_libs() {   # host_libs DIR: what the ELF files under DIR need and DIR does not hold
    local dir=$1 f
    comm -23 \
        <(elf_files "$dir" | while IFS= read -r f; do
              objdump -p "$f" | awk '$1 == "NEEDED" { print $2 }'; done | sort -u) \
        <(find "$dir" -name '*.so*' -printf '%f\n' | sort -u) | tr '\n' ' '
}
floor_report() {  # floor_report TITLE DIR: per file, then the maximum, into glibc_floor.txt
    local title=$1 dir=$2 f
    mapfile -t elves < <(elf_files "$dir" | sort)
    {
        echo "$title"
        for f in "${elves[@]}"; do
            printf '  %-60s GLIBC %-6s GLIBCXX %s\n' "${f#"$dir"/}" \
                "$(needs GLIBC "$f")" "$(needs GLIBCXX "$f")"
        done
        echo "  from the system: $(host_libs "$dir")"
        echo "  => ${#elves[@]} ELF files; floor: GLIBC_$(needs GLIBC "${elves[@]}")," \
             "GLIBCXX_$(needs GLIBCXX "${elves[@]}"), CXXABI_$(needs CXXABI "${elves[@]}")"
        echo
    } >> "$OUT/glibc_floor.txt"
}

render() {      # render TEMPLATE OUTPUT GLIBC GLIBCXX
    sed -e "s|@VERSION@|$VERSION|g" -e "s|@QT_VERSION@|$QT_VERSION|g" \
        -e "s|@GLIBC@|$3|g" -e "s|@GLIBCXX@|$4|g" "$1" > "$2"
}

pack() {        # pack DIR NAME: tar.gz of $DIR/$NAME, owned by root, in name order
    tar -C "$1" --owner=0 --group=0 --numeric-owner --sort=name -czf "$OUT/$2.tar.gz" "$2"
}

rm -f "$OUT/glibc_floor.txt"
STAGE=$OUT/stage
rm -rf "$STAGE"
mkdir -p "$STAGE"

# ---- standalone: AppDir ----------------------------------------------------------
#
# SDL2 goes in although nothing links it: the app dlopen()s libSDL2-2.0.so.0 for
# sound, and that lookup honours the executable's RUNPATH ($ORIGIN/../lib, which
# linuxdeploy sets), so the bundled copy is found first. Without one the app
# runs silently.

APPDIR=$OUT/AppDir
rm -rf "$APPDIR"
SDL_LIB=$( { ldconfig -p 2>/dev/null || /sbin/ldconfig -p; } \
           | awk '/libSDL2-2\.0\.so\.0 .*x86-64/ {print $NF; exit}' )
extra=()
if [ -n "$SDL_LIB" ]; then
    extra+=(--library "$SDL_LIB")
else
    echo "warning: no libSDL2-2.0.so.0 on this machine; the package will rely on the user's" >&2
fi

# One run deploys the AppDir and squashes it into the AppImage, so the tarball
# below, copied from the AppDir afterwards, holds exactly what the AppImage does.
APPIMAGE=smd_dgx-linux-x86_64.AppImage
rm -f "$OUT/$APPIMAGE"
say "AppDir and $APPIMAGE: linuxdeploy + Qt plugin (Qt $QT_VERSION from $QMAKE)"
(cd "$OUT" && LDAI_OUTPUT=$APPIMAGE "$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$BUILD/smd_dgx_qt6" \
    --desktop-file "$ASSETS/smd_dgx.desktop" \
    --icon-file "$ASSETS/smd_dgx.png" \
    "${extra[@]}" \
    --plugin qt --output appimage)
[ -e "$APPDIR/AppRun" ]   || die "linuxdeploy left no AppRun in $APPDIR"
[ -f "$OUT/$APPIMAGE" ]   || die "linuxdeploy did not write $OUT/$APPIMAGE"

floor_report "standalone (AppDir: the tarball and the AppImage's contents)" "$APPDIR"
mapfile -t app_elves < <(elf_files "$APPDIR")
APP_GLIBC=$(needs GLIBC "${app_elves[@]}")
APP_GLIBCXX=$(needs GLIBCXX "${app_elves[@]}")

# ---- standalone: tarball -------------------------------------------------------------

NAME=smd_dgx_standalone_linux_x86_64
say "$NAME.tar.gz"
cp -a "$APPDIR" "$STAGE/$NAME"
render "$ASSETS/README.txt" "$STAGE/$NAME/README.txt" "$APP_GLIBC" "$APP_GLIBCXX"
cp "$ROOT/LICENSE.txt" "$STAGE/$NAME/LICENSE.txt"
pack "$STAGE" "$NAME"
cp "$STAGE/$NAME/README.txt" "$OUT/smd_dgx_linux_README.txt"

# ---- IDA modules ------------------------------------------------------------------

if [ -n "${IDA_SDK_DIR:-}" ]; then
    INAME=smd_dgx_ida_linux_x86_64
    say "$INAME.tar.gz"
    I=$STAGE/$INAME
    mkdir -p "$I/plugins" "$I/loaders"
    cp "$BUILD/ida_plugin/smd_dgx_ida.so" "$I/plugins/"
    cp "$BUILD/ida_loader/smd_dgx_ldr.so" "$BUILD/ida_loader/smd_dgx_z80_ldr.so" "$I/loaders/"
    strip --strip-unneeded "$I"/plugins/*.so "$I"/loaders/*.so
    cp -r "$SRC/mcp" "$I/mcp"
    find "$I/mcp" \( -name __pycache__ -o -name '*.pyc' \) -prune -exec rm -rf {} +
    cp "$ROOT/LICENSE.txt" "$I/LICENSE.txt"
    floor_report "IDA modules" "$I"
    mapfile -t ida_elves < <(elf_files "$I")
    render "$ASSETS/INSTALL.txt" "$I/INSTALL.txt" \
        "$(needs GLIBC "${ida_elves[@]}")" "$(needs GLIBCXX "${ida_elves[@]}")"
    pack "$STAGE" "$INAME"
fi

# ---- summary --------------------------------------------------------------------

rm -rf "$STAGE"
say "floor (objdump -T, imported versions only; per file in glibc_floor.txt)"
grep -E '^[^ ]|=>|from the system' "$OUT/glibc_floor.txt"
say "packages in $OUT ($VERSION)"
cd "$OUT"
for f in "$APPIMAGE" "$NAME.tar.gz" ${INAME:+"$INAME.tar.gz"}; do
    printf '%10s  %s  %s\n' "$(du -h "$f" | cut -f1)" "$(sha256sum "$f" | cut -c1-16)" "$f"
done
