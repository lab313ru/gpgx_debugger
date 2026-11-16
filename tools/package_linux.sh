#!/usr/bin/env bash
# Build SMD DGX on Linux and package it for people who will not build it.
#
# Writes to builds/linux-package/ (or --out DIR):
#   smd_dgx-linux-x86_64.AppImage            the standalone app as a single file
#   smd_dgx_standalone_linux_x86_64.tar.gz   the same AppDir unpacked, AppRun and
#                                            all, for machines without FUSE
#   smd_dgx_linux_README.txt                 the README and the licence from the
#   smd_dgx_linux_LICENSE.txt                package, to put next to the AppImage
#   smd_dgx_ida_linux_x86_64.tar.gz          the IDA plugin, both loaders, mcp/
#   glibc_floor.txt                          what each binary needs from the host
#
# Run it on Linux from a clone of the repository (it has been run on Ubuntu
# 24.04 under WSL2). It needs cmake, ninja, g++, Qt 6 with qmake6, the SDL2
# headers, and, for the IDA archive, the IDA SDK:
#
#   IDA_SDK_DIR=<ida-sdk>/src tools/package_linux.sh
#
# Without IDA_SDK_DIR the IDA archive is skipped.
#
# linuxdeploy and its Qt plugin do the deploying, and this script downloads
# neither. Put them into $SMD_DGX_TOOLS_DIR (default ~/.cache/smd_dgx/tools)
# under their release names, or point LINUXDEPLOY and LINUXDEPLOY_PLUGIN_QT at
# them. They decide which libraries go into the package and which are left to
# the system, and the AppImage runtime is cut out of linuxdeploy, so another
# build of them makes another package: tools/linux/linuxdeploy.sha256 names
# the copies the packages are made with, and any other copy is refused unless
# SMD_DGX_ANY_LINUXDEPLOY=1. Both are AppImages; they run with
# APPIMAGE_EXTRACT_AND_RUN=1, so packaging needs no FUSE.
#
# Options:
#   --out DIR       where the packages go (default: <repo>/builds/linux-package)
#   --skip-tests    do not build and run the test suite first
#
# Environment:
#   IDA_SDK_DIR             the SDK's src/ directory (the one with include/ida.hpp)
#   QMAKE                   Qt 6 qmake to deploy from (default: qmake6 on PATH)
#   SMD_DGX_VERSION         the version line in the READMEs (default: git describe)
#   SMD_DGX_TOOLS_DIR       where linuxdeploy and its Qt plugin are looked for
#   LINUXDEPLOY             linuxdeploy-x86_64.AppImage, if not in that directory
#   LINUXDEPLOY_PLUGIN_QT   linuxdeploy-plugin-qt-x86_64.AppImage, likewise
#   SMD_DGX_ANY_LINUXDEPLOY 1: accept copies other than the pinned ones
set -euo pipefail
shopt -u patsub_replacement 2>/dev/null || true    # '&' in a value stays '&'

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
[ "$(uname -m)" = x86_64 ] || die "x86_64 only: linuxdeploy runs as an x86_64 AppImage"
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
DISTRO=$( . /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-$ID}" || uname -sr)

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- linuxdeploy ---------------------------------------------------------------

TOOLS=${SMD_DGX_TOOLS_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/smd_dgx/tools}
LINUXDEPLOY=${LINUXDEPLOY:-$TOOLS/linuxdeploy-x86_64.AppImage}
LINUXDEPLOY_PLUGIN_QT=${LINUXDEPLOY_PLUGIN_QT:-$TOOLS/linuxdeploy-plugin-qt-x86_64.AppImage}
PINS=$ASSETS/linuxdeploy.sha256
MADE_WITH=()

tool() {        # tool NAME PATH VAR: say which copy of NAME this is; die unless pinned
    local name=$1 f=$2 var=$3 want got note
    [ -f "$f" ] || die "no $name at $f
  This script downloads nothing. $name-x86_64.AppImage comes from
  https://github.com/linuxdeploy/$name/releases; put it into
  $TOOLS or set $var to its path."
    [ -x "$f" ] || die "$f is not executable (chmod +x it)"
    got=$(sha256sum "$f" | cut -d' ' -f1)
    want=$(awk -v n="$name" '$1 !~ /^#/ && $2 == n { print $1 }' "$PINS")
    echo "$name: $f"
    echo "  sha256    $got"
    if [ "$got" = "$want" ]; then
        note="the copy tools/linux/linuxdeploy.sha256 pins"
    elif [ "${SMD_DGX_ANY_LINUXDEPLOY:-0}" = 1 ]; then
        note="NOT the pinned copy ($want), taken because SMD_DGX_ANY_LINUXDEPLOY=1"
    else
        die "$f is not the $name that tools/linux/linuxdeploy.sha256 pins:
  want $want.
  Another build deploys other libraries and another runtime;
  SMD_DGX_ANY_LINUXDEPLOY=1 takes it anyway and says so in BUNDLED.txt."
    fi
    echo "  $note"
    MADE_WITH+=("$(printf '  %-22s sha256 %s\n  %-22s %s' "$name" "$got" "" "$note")")
}

say "linuxdeploy (looked for in $TOOLS)"
tool linuxdeploy           "$LINUXDEPLOY"           LINUXDEPLOY
tool linuxdeploy-plugin-qt "$LINUXDEPLOY_PLUGIN_QT" LINUXDEPLOY_PLUGIN_QT

# The AppImage runtime. Given none, appimagetool downloads one from GitHub. It
# need not: linuxdeploy is an AppImage itself, and its first --appimage-offset
# bytes are exactly that runtime. Its update-information slot names
# linuxdeploy's own releases, though, and an AppImage updater would then
# replace our AppImage with linuxdeploy, so that slot is zeroed.
RUNTIME=$WORK/runtime-x86_64
off=$("$LINUXDEPLOY" --appimage-offset)
head -c "$off" "$LINUXDEPLOY" > "$RUNTIME"
read -r upd_off upd_size < <(readelf -S -W "$RUNTIME" \
    | awk '{ for (i = 1; i < NF; i++) if ($i == ".upd_info") { print $(i+3), $(i+4); exit } }') || true
[ -n "${upd_size:-}" ] || die "no .upd_info section in the runtime taken from $LINUXDEPLOY"
dd if=/dev/zero of="$RUNTIME" bs=1 seek=$((16#$upd_off)) count=$((16#$upd_size)) \
   conv=notrunc status=none
echo "runtime:    the first $off bytes of linuxdeploy, .upd_info zeroed"
echo "  sha256    $(sha256sum "$RUNTIME" | cut -d' ' -f1)"
MADE_WITH+=("$(printf '  %-22s sha256 %s\n  %-22s %s' "AppImage runtime" \
    "$(sha256sum "$RUNTIME" | cut -d' ' -f1)" "" "the first $off bytes of linuxdeploy, .upd_info zeroed")")
export LDAI_RUNTIME_FILE=$RUNTIME

export APPIMAGE_EXTRACT_AND_RUN=1      # no FUSE needed to run the tools
export QMAKE
# The tools unpack themselves into $TMPDIR and leave a copy behind there on
# every run; in the work directory it goes when the script ends.
export TMPDIR=$WORK

# linuxdeploy finds its plugins by name on PATH, so a link under the release
# name goes first. It also dies on the first PATH entry it may not read, and
# WSL appends the whole Windows PATH, some of it unreadable from Linux; nothing
# here needs Windows programs, so those entries go.
mkdir -p "$WORK/bin"
ln -s "$(realpath "$LINUXDEPLOY_PLUGIN_QT")" "$WORK/bin/linuxdeploy-plugin-qt-x86_64.AppImage"
path=$WORK/bin
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

# ---- what the standalone takes from the system -----------------------------------
#
# Followed from two roots through the bundled libraries: the app with its Qt
# plugins, which does not start without what they reach, and libSDL2, which is
# only dlopen()ed for sound. The C and C++ runtime is left out of both lists;
# the glibc and GLIBCXX floor speaks for it.

RUNTIME_LIBS='^(ld-linux.*|libc\.so\.6|libm\.so\.6|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1)$'

from_system() { # from_system LIBDIR ROOT...: what the ROOTs reach through LIBDIR that it lacks
    local libdir=$1 f n
    shift
    local -a queue=("$@")
    local -A seen=() host=()
    while [ ${#queue[@]} -gt 0 ]; do
        f=${queue[0]}
        queue=("${queue[@]:1}")
        [ -z "${seen[$f]+x}" ] || continue
        seen[$f]=1
        for n in $(objdump -p "$f" | awk '$1 == "NEEDED" { print $2 }'); do
            if [ -e "$libdir/$n" ]; then queue+=("$libdir/$n"); else host[$n]=1; fi
        done
    done
    for n in "${!host[@]}"; do printf '%s\n' "$n"; done | grep -vE "$RUNTIME_LIBS" | sort || true
}
# The readers below never stop early: under pipefail, a writer cut off by
# SIGPIPE fails the whole pipeline, and set -e then ends the script.
ldcache() { ldconfig -p 2>/dev/null || /sbin/ldconfig -p; }
lib_path() {    # lib_path SONAME: where this machine has it
    ldcache | awk -v n="$1" '$1 == n && /x86-64/ && p == "" { p = $NF } END { if (p != "") print p }'
}
deb_of() {      # deb_of PATH: the package that installed it here, or nothing
    local p=$1
    command -v dpkg >/dev/null || return 0
    { dpkg -S "$p" 2>/dev/null || dpkg -S "$(realpath "$p")" 2>/dev/null \
        || { [ "${p#/usr/}" = "$p" ] && dpkg -S "/usr$p" 2>/dev/null; } || true; } \
        | sed -n '1{s/: .*//; s/:[a-z0-9_]*$//; s/, .*//; p}'
}
debs_for() {    # debs_for SONAME...: the packages that hold them on this machine
    local n p
    for n in "$@"; do
        p=$(lib_path "$n")
        [ -z "$p" ] || deb_of "$p"
    done | sort -u
}
wrap() {        # wrap WORD...: four-space indented lines, or a placeholder
    [ $# -gt 0 ] || { echo "    (none)"; return 0; }
    printf '%s ' "$@" | fold -s -w 68 | sed 's/ *$//; s/^/    /'
}
wrap_debs() {   # wrap_debs PACKAGE...: as wrap, unless there was no dpkg to ask
    if command -v dpkg >/dev/null; then wrap "$@"; else echo "    (not known: no dpkg on the build machine)"; fi
}
bundled_report() {  # bundled_report DIR: each library in usr/lib and usr/plugins, by package
    local dir=$1 qtplugins f rel p pkg
    qtplugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)
    command -v dpkg-query >/dev/null || {
        echo "  (no dpkg on the build machine: the packages are not recorded)"; return 0; }
    elf_files "$dir/usr/lib" "$dir/usr/plugins" | sort | while IFS= read -r f; do
        rel=${f#"$dir"/}
        # Not every library is in the linker cache (libpulsecommon lives in a
        # subdirectory); dpkg -S also takes a pattern.
        case $rel in
            usr/plugins/*) p=$qtplugins/${rel#usr/plugins/} ;;
            *)             p=$(lib_path "${f##*/}"); p=${p:-"*/${f##*/}"} ;;
        esac
        pkg=$([ -n "$p" ] && deb_of "$p" || true)
        if [ -n "$pkg" ]; then
            printf '  %-56s %s\n' "$rel" \
                "$(dpkg-query -W -f='${Package} ${Version}  (source: ${source:Package} ${source:Version})' "$pkg")"
        else
            printf '  %-56s %s\n' "$rel" "? (not found in the package database)"
        fi
    done
}

render() {      # render TEMPLATE OUTPUT KEY=VALUE...: every @KEY@ becomes VALUE
    local text kv key
    text=$(<"$1")
    for kv in "${@:3}"; do
        key=${kv%%=*}
        text=${text//"@$key@"/"${kv#*=}"}
    done
    printf '%s\n' "$text" > "$2"
    if grep -qE '@[A-Z_]+@' "$2"; then
        die "$2: placeholder left unfilled: $(grep -oE '@[A-Z_]+@' "$2" | head -n 1)"
    fi
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
SDL_LIB=$(lib_path libSDL2-2.0.so.0)
extra=()
if [ -n "$SDL_LIB" ]; then
    extra+=(--library "$SDL_LIB")
else
    echo "warning: no libSDL2-2.0.so.0 on this machine; the package will rely on the user's" >&2
fi

# Two runs. The first deploys the AppDir; the README, measured over what it
# deployed, the licence and the library list then go in beside AppRun; the
# second squashes all of it into the AppImage. The tarball, copied from the
# AppDir afterwards, holds exactly what the AppImage does.
APPIMAGE=smd_dgx-linux-x86_64.AppImage
rm -f "$OUT/$APPIMAGE"
say "AppDir: linuxdeploy + Qt plugin (Qt $QT_VERSION from $QMAKE)"
(cd "$OUT" && "$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$BUILD/smd_dgx_qt6" \
    --desktop-file "$ASSETS/smd_dgx.desktop" \
    --icon-file "$ASSETS/smd_dgx.png" \
    "${extra[@]}" \
    --plugin qt)
[ -e "$APPDIR/AppRun" ] || die "linuxdeploy left no AppRun in $APPDIR"

floor_report "standalone (AppDir: the tarball and the AppImage's contents)" "$APPDIR"
mapfile -t app_elves < <(elf_files "$APPDIR")
APP_GLIBC=$(needs GLIBC "${app_elves[@]}")
APP_GLIBCXX=$(needs GLIBCXX "${app_elves[@]}")

mapfile -t app_roots < <(elf_files "$APPDIR/usr/bin" "$APPDIR/usr/plugins")
mapfile -t start_libs < <(from_system "$APPDIR/usr/lib" "${app_roots[@]}")
sound_libs=()
if [ -e "$APPDIR/usr/lib/libSDL2-2.0.so.0" ]; then
    mapfile -t sound_libs < <(comm -23 \
        <(from_system "$APPDIR/usr/lib" "$APPDIR/usr/lib/libSDL2-2.0.so.0") \
        <(printf '%s\n' "${start_libs[@]}"))
fi
mapfile -t start_debs < <(debs_for "${start_libs[@]}")
mapfile -t sound_debs < <(debs_for "${sound_libs[@]}")
echo "from the system, to start: ${start_libs[*]}"
echo "from the system, for sound only: ${sound_libs[*]}"

render "$ASSETS/README.txt" "$APPDIR/README.txt" \
    "VERSION=$VERSION" "QT_VERSION=$QT_VERSION" \
    "GLIBC=$APP_GLIBC" "GLIBCXX=$APP_GLIBCXX" \
    "START_LIBS=$(wrap "${start_libs[@]}")" "START_DEBS=$(wrap_debs "${start_debs[@]}")" \
    "SOUND_LIBS=$(wrap "${sound_libs[@]}")" "SOUND_DEBS=$(wrap_debs "${sound_debs[@]}")"
cp "$ROOT/LICENSE.txt" "$APPDIR/LICENSE.txt"
{
    echo "SMD DGX $VERSION, packaged by tools/package_linux.sh"
    echo "on $DISTRO."
    echo
    echo "Made with:"
    printf '%s\n' "${MADE_WITH[@]}"
    echo
    echo "usr/lib and usr/plugins: each file, the package of that distribution it"
    echo "was copied from, and that package's source package. linuxdeploy copied"
    echo "them, pointed their RUNPATH into usr/lib and ran strip over them;"
    echo "nothing else was changed. Their sources are the distribution's source"
    echo "packages of these versions (apt-get source <name>=<version>), their"
    echo "copyright files are in usr/share/doc."
    echo
    bundled_report "$APPDIR"
} > "$APPDIR/BUNDLED.txt"

say "$APPIMAGE: linuxdeploy again, squashing the AppDir"
(cd "$OUT" && LDAI_OUTPUT=$APPIMAGE "$LINUXDEPLOY" --appdir "$APPDIR" --output appimage)
[ -f "$OUT/$APPIMAGE" ] || die "linuxdeploy did not write $OUT/$APPIMAGE"

# ---- standalone: tarball -------------------------------------------------------------

NAME=smd_dgx_standalone_linux_x86_64
say "$NAME.tar.gz"
cp -a "$APPDIR" "$STAGE/$NAME"
pack "$STAGE" "$NAME"
cp "$APPDIR/README.txt"  "$OUT/smd_dgx_linux_README.txt"
cp "$APPDIR/LICENSE.txt" "$OUT/smd_dgx_linux_LICENSE.txt"

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
    render "$ASSETS/INSTALL.txt" "$I/INSTALL.txt" "VERSION=$VERSION" \
        "GLIBC=$(needs GLIBC "${ida_elves[@]}")" "GLIBCXX=$(needs GLIBCXX "${ida_elves[@]}")"
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
