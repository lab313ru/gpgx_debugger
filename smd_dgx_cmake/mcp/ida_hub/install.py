"""Install ida-hub's IDA-side pieces.

Two things go into IDA's user plugin folder:

  mcp_autostart.py   starts ida-pro-mcp's server as soon as a database is
                     open, so nobody has to press Ctrl-Alt-M per instance.

  mcp-plugin.py      the ida-pro-mcp plugin itself, vendored here (MIT) with
                     one fix: SO_REUSEADDR -> SO_EXCLUSIVEADDRUSE on Windows.
                     An installed plugin is patched in place; if none is
                     installed, the vendored copy goes in whole.

     On Windows SO_REUSEADDR lets a SECOND process bind a port that is
     already listening. bind() then succeeds for every IDA, the plugin's
     port-search loop never runs, and every instance reports 13337 while
     only one actually receives connections. That is precisely the
     "several IDAs, agent sees one" symptom this hub exists to fix, and no
     amount of client-side scanning can see a server that never got its
     own port. Reinstalling ida-pro-mcp overwrites the plugin, so the fix
     is a script and not a memory.

Usage:
    python install.py            install both
    python install.py --check    report state, change nothing
    python install.py --remove   remove the companion, revert the fix
"""

from __future__ import annotations

import io
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMPANION = "mcp_autostart.py"
VENDORED = os.path.join(HERE, "mcp-plugin.py")   # upstream 1.5.0 + the fix

if sys.platform == "win32":
    PLUGIN_DIR = os.path.join(os.environ.get("APPDATA", ""), "Hex-Rays", "IDA Pro", "plugins")
else:
    PLUGIN_DIR = os.path.join(os.path.expanduser("~"), ".idapro", "plugins")

PLUGIN = os.path.join(PLUGIN_DIR, "mcp-plugin.py")

OLD_LINE = "            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
FIX_MARK = "SO_EXCLUSIVEADDRUSE"
FIX = '''            # SO_REUSEADDR means something different on Windows: it lets a
            # SECOND process bind a port that is already listening. bind()
            # then succeeds for every IDA, the port-search loop below never
            # runs, and several instances all report 13337 while only one
            # actually receives connections. SO_EXCLUSIVEADDRUSE is the
            # Windows option that makes a busy port fail the way the loop
            # expects. On POSIX SO_REUSEADDR only skips TIME_WAIT, which is
            # what we want there.  (patched by ida-hub/install.py)
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            else:
                self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
'''


def _read(p: str) -> str:
    return io.open(p, encoding="utf-8").read()


def _write(p: str, s: str) -> None:
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def check() -> tuple[bool, bool, str]:
    """(companion installed, plugin patched, note)"""
    companion = os.path.exists(os.path.join(PLUGIN_DIR, COMPANION))
    if not os.path.exists(PLUGIN):
        return companion, False, f"ida-pro-mcp plugin not found at {PLUGIN}"
    src = _read(PLUGIN)
    if FIX_MARK in src:
        return companion, True, "patched"
    if OLD_LINE in src:
        return companion, False, "unpatched (original SO_REUSEADDR line present)"
    return companion, False, "plugin layout not recognised — patch by hand or update install.py"


def install() -> int:
    os.makedirs(PLUGIN_DIR, exist_ok=True)
    shutil.copy(os.path.join(HERE, COMPANION), os.path.join(PLUGIN_DIR, COMPANION))
    print(f"installed {COMPANION} -> {PLUGIN_DIR}")

    _, patched, note = check()
    if patched:
        print("mcp-plugin.py: already patched")
        return 0
    if not os.path.exists(PLUGIN):
        # Nothing installed: the vendored, already-fixed copy goes in whole.
        # It still needs the ida_pro_mcp Python package for its own imports.
        shutil.copy(VENDORED, PLUGIN)
        print(f"mcp-plugin.py: installed vendored copy -> {PLUGIN}")
        print("  (needs `pip install ida-pro-mcp` for the plugin's imports)")
        print("restart IDA for both to take effect")
        return 0
    src = _read(PLUGIN)
    if OLD_LINE not in src:
        # Some other version. Do not guess at its layout: say so and stop.
        print(f"mcp-plugin.py: {note}")
        print(f"  the vendored copy at {VENDORED} is upstream 1.5.0 with the fix;")
        print("  update this installer for the new layout, or install that copy.")
        return 1
    _write(PLUGIN, src.replace(OLD_LINE, FIX, 1))
    print("mcp-plugin.py: patched (SO_REUSEADDR -> SO_EXCLUSIVEADDRUSE on Windows)")
    print("restart IDA for both to take effect")
    return 0


def remove() -> int:
    p = os.path.join(PLUGIN_DIR, COMPANION)
    if os.path.exists(p):
        os.remove(p)
        print(f"removed {p}")
    if os.path.exists(PLUGIN):
        src = _read(PLUGIN)
        if FIX in src:
            _write(PLUGIN, src.replace(FIX, OLD_LINE, 1))
            print("mcp-plugin.py: fix reverted")
    return 0


if __name__ == "__main__":
    if "--check" in sys.argv:
        c, p, note = check()
        print(f"companion: {'installed' if c else 'absent'}   plugin: {note}")
        sys.exit(0)
    sys.exit(remove() if "--remove" in sys.argv else install())
