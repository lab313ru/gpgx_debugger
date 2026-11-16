#!/usr/bin/env python3
"""Package a Windows build of SMD DGX for people who will not build it.

    python tools/package_windows.py --build builds/x64-release --version v1.9 --out <dir>
                                    [--date YYYY-MM-DD] [--previous <dir> --previous-version v1.8.1]

Writes to --out:
    smd_dgx_standalone_<version>.zip   the standalone app with its Qt, unpack and run
    smd_dgx_ida_<version>.zip          the IDA plugin, both loaders and mcp/, INSTALL.txt

Every file is named here, not globbed from the build directory: the Qt deploy
step leaves more there than the app loads (qsvg.dll without Qt6Svg.dll, for
one), and a missing file fails the run instead of shipping a broken archive.
SDL2.dll is not shipped: the standalone plays through waveOut.

With --previous, the file lists are compared with that release's archives
(smd_dgx_*_<previous-version>.zip in that directory) and every file added or
removed is printed, so a change in what ships is a decision, not an accident.
"""
import argparse
import datetime
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "smd_dgx_cmake")
TEMPLATES = os.path.join(ROOT, "tools", "windows")

STANDALONE = [
    "smd_dgx_qt6.exe",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Network.dll", "Qt6Widgets.dll",
    "d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll",
    "generic/qtuiotouchplugin.dll",
    "imageformats/qgif.dll", "imageformats/qico.dll", "imageformats/qjpeg.dll",
    "networkinformation/qnetworklistmanager.dll",
    "platforms/qwindows.dll",
    "styles/qmodernwindowsstyle.dll",
    "tls/qcertonlybackend.dll", "tls/qschannelbackend.dll",
]

# (path in the archive, path relative to the build directory or to SRC)
IDA_FROM_BUILD = [
    ("plugins/smd_dgx_ida.dll", "ida_plugin/smd_dgx_ida.dll"),
    ("loaders/smd_dgx_ldr.dll", "ida_loader/smd_dgx_ldr.dll"),
    ("loaders/smd_dgx_z80_ldr.dll", "ida_loader/smd_dgx_z80_ldr.dll"),
]
IDA_FROM_SRC = [
    ("mcp/smd_dgx_mcp.py", "mcp/smd_dgx_mcp.py"),
    ("mcp/ida_hub/ida_hub.py", "mcp/ida_hub/ida_hub.py"),
    ("mcp/ida_hub/mcp_autostart.py", "mcp/ida_hub/mcp_autostart.py"),
    ("mcp/ida_hub/install.py", "mcp/ida_hub/install.py"),
    ("mcp/ida_hub/mcp-plugin.py", "mcp/ida_hub/mcp-plugin.py"),
    ("mcp/ida_hub/LICENSE-ida-pro-mcp", "mcp/ida_hub/LICENSE-ida-pro-mcp"),
]


def text(template, version, date):
    with open(os.path.join(TEMPLATES, template), encoding="utf-8") as f:
        s = f.read()
    for key, value in (("@VERSION@", version), ("@DATE@", date)):
        if key not in s:
            sys.exit("package_windows: %s has no %s" % (template, key))
        s = s.replace(key, value)
    return s.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def write_zip(path, files, texts):
    """files: [(name in archive, source path)]; texts: [(name, bytes)]."""
    missing = [src for _, src in files if not os.path.isfile(src)]
    if missing:
        sys.exit("package_windows: missing from the build:\n  " + "\n  ".join(missing))
    if os.path.exists(path):
        os.remove(path)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in texts:
            z.writestr(name, data)
        for name, src in files:
            z.write(src, name)
    print("%-40s %10d bytes, %d files" % (os.path.basename(path), os.path.getsize(path),
                                          len(files) + len(texts)))


def names(path):
    with zipfile.ZipFile(path) as z:
        return {n.replace("\\", "/") for n in z.namelist() if not n.endswith("/")}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", required=True, help="the x64-release build directory")
    ap.add_argument("--version", required=True, help="e.g. v1.9")
    ap.add_argument("--out", required=True)
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    ap.add_argument("--previous", help="directory with the previous release's archives")
    ap.add_argument("--previous-version")
    a = ap.parse_args()

    build = os.path.abspath(a.build)
    os.makedirs(a.out, exist_ok=True)
    sa = os.path.join(a.out, "smd_dgx_standalone_%s.zip" % a.version)
    ida = os.path.join(a.out, "smd_dgx_ida_%s.zip" % a.version)

    write_zip(sa, [(n, os.path.join(build, *n.split("/"))) for n in STANDALONE],
              [("README.txt", text("README.txt", a.version, a.date))])
    write_zip(ida,
              [(n, os.path.join(build, *s.split("/"))) for n, s in IDA_FROM_BUILD]
              + [(n, os.path.join(SRC, *s.split("/"))) for n, s in IDA_FROM_SRC],
              [("INSTALL.txt", text("INSTALL.txt", a.version, a.date))])

    if a.previous:
        if not a.previous_version:
            sys.exit("package_windows: --previous needs --previous-version")
        for kind, new in (("standalone", sa), ("ida", ida)):
            old = os.path.join(a.previous, "smd_dgx_%s_%s.zip" % (kind, a.previous_version))
            o, n = names(old), names(new)
            print("%-10s added %s, removed %s" % (kind, sorted(n - o) or "nothing",
                                                   sorted(o - n) or "nothing"))


if __name__ == "__main__":
    main()
