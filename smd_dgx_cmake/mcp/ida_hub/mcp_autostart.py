"""mcp_autostart — start ida-pro-mcp's server the moment a database is open.

The MCP plugin deliberately does nothing in init(): its server only starts from
run(), i.e. Edit -> Plugins -> MCP or Ctrl-Alt-M, once per IDA, by hand. Open
three databases and you press it three times; forget one and an agent sees two
IDAs where there are three.

This companion waits for the database to be ready, then invokes the MCP plugin
exactly as the hotkey would. It runs on IDA's own thread — the plugin's start()
spawns its server thread itself, so nothing here blocks the UI.

Install: copy next to mcp-plugin.py, i.e.
    Windows:  %APPDATA%\\Hex-Rays\\IDA Pro\\plugins\\
    else:     ~/.idapro/plugins/

To turn it off without removing it:  set IDA_MCP_AUTOSTART=0
"""

import os

import ida_kernwin
import idaapi

# find_plugin() matches the FILE name without extension, not wanted_name
# (loader.hpp: "short plugin name without path and extension"). "MCP" would
# silently find nothing.
PLUGIN_FILE = "mcp-plugin"


class _UIHooks(ida_kernwin.UI_Hooks):
    """ready_to_run fires once the database is loaded and the UI is up — the
    earliest moment run_plugin() is safe to call."""

    def __init__(self, on_ready):
        super().__init__()
        self._on_ready = on_ready
        self._fired = False

    def ready_to_run(self):
        if self._fired:
            return
        self._fired = True
        self._on_ready()


class MCPAutostart(idaapi.plugin_t):
    flags = idaapi.PLUGIN_FIX | idaapi.PLUGIN_HIDE
    comment = "Starts the ida-pro-mcp server automatically when a database opens"
    help = ""
    wanted_name = "MCP autostart"
    wanted_hotkey = ""

    def init(self):
        if os.environ.get("IDA_MCP_AUTOSTART", "1") == "0":
            return idaapi.PLUGIN_SKIP
        self._hooks = _UIHooks(self._start_mcp)
        self._hooks.hook()
        return idaapi.PLUGIN_KEEP

    def _start_mcp(self):
        # load_and_run_plugin loads mcp-plugin.py if it is not resident yet and
        # calls its run(), which is exactly what the hotkey does. arg=0 matches
        # the hotkey path. If the plugin is missing, IDA prints why and we do
        # nothing more — no retries, no dialogs.
        try:
            if not idaapi.load_and_run_plugin(PLUGIN_FILE, 0):
                print(f"[MCP autostart] {PLUGIN_FILE} not found or refused to run "
                      "(is ida-pro-mcp installed?)")
        except Exception as e:  # noqa: BLE001 — never let a helper crash IDA
            print(f"[MCP autostart] could not start {PLUGIN_FILE}: {e}")

    def run(self, arg):
        # Manual invocation: just start it now.
        self._start_mcp()

    def term(self):
        try:
            self._hooks.unhook()
        except Exception:
            pass


def PLUGIN_ENTRY():
    return MCPAutostart()
