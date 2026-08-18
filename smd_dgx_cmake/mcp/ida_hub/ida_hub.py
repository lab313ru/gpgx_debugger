"""ida-hub — one MCP endpoint in front of every running IDA.

The ida-pro-mcp plugin already handles several IDA instances on its own side:
each one takes the next free port from 13337 upward. What is single-instance is
the CLIENT — a hardcoded http://127.0.0.1:13337/mcp — so an agent only ever
sees the first IDA that started, and cannot say which game it wants.

This server sits in front. It scans the port range, asks each IDA who it is
(the plugin's own get_metadata: file name and hashes), and exposes every tool
of the SELECTED IDA transparently — the tool list is fetched live from the
plugin, not copied by hand, so a plugin upgrade needs no change here.

    pip install mcp
    python ida_hub.py            # stdio MCP server, name "ida-hub"

Tools of its own:
    list_ida_sessions              every IDA answering, with its file
    use_ida_session(target)        pick one by port, file name, or substring
    current_ida_session            which one calls go to now

Everything else — decompile_function, get_xrefs_to, ... — is the plugin's,
forwarded unchanged to whichever IDA is selected.
"""

from __future__ import annotations

import http.client
import json
import os
import socket
import sys
import threading
from typing import Any

from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.tools import Tool

HOST = os.environ.get("IDA_MCP_HOST", "127.0.0.1")
BASE_PORT = int(os.environ.get("IDA_MCP_BASE_PORT", "13337"))
PORT_RANGE = int(os.environ.get("IDA_MCP_PORT_RANGE", "10"))   # the plugin tries 10

mcp = FastMCP("ida-hub")


# ---------------------------------------------------------------------------
# talking to one IDA
# ---------------------------------------------------------------------------
_rpc_id = 0
_lock = threading.Lock()


def _rpc(port: int, method: str, params: dict[str, Any] | None = None,
         timeout: float = 30.0) -> Any:
    """One JSON-RPC round trip to the plugin's /mcp endpoint."""
    global _rpc_id
    with _lock:
        _rpc_id += 1
        rid = _rpc_id
    body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                       "params": params or {}})
    conn = http.client.HTTPConnection(HOST, port, timeout=timeout)
    try:
        conn.request("POST", "/mcp", body=body,
                     headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        data = json.loads(resp.read().decode("utf-8", errors="replace"))
    finally:
        conn.close()
    if "error" in data:
        err = data["error"]
        detail = err.get("data") or ""
        raise RuntimeError(f"IDA on {port}: {err.get('message')} {detail}".strip())
    return data.get("result")


def _call_tool(port: int, name: str, arguments: dict[str, Any],
               timeout: float = 30.0) -> Any:
    """tools/call, unwrapping the plugin's content envelope back into a value."""
    result = _rpc(port, "tools/call", {"name": name, "arguments": arguments},
                  timeout=timeout)
    if isinstance(result, dict) and "content" in result:
        texts = [c.get("text", "") for c in result["content"] if c.get("type") == "text"]
        joined = "\n".join(texts)
        # The plugin json-dumps structured results; hand them back structured.
        try:
            return json.loads(joined)
        except (ValueError, TypeError):
            return joined
    return result


def _identity(port: int) -> dict[str, str] | None:
    """Who is on this port, or None if nothing of ours answers.

    Short timeout: a closed loopback port refuses instantly, and an IDA that
    takes seconds to answer get_metadata is not one we want to wait on ten
    times over during a scan.
    """
    try:
        meta = _call_tool(port, "get_metadata", {}, timeout=1.5)
    except Exception:
        return None
    if not isinstance(meta, dict):
        return None
    path = str(meta.get("path") or "")
    return {
        "port":   str(port),
        "file":   os.path.basename(path) or str(meta.get("module") or ""),
        "path":   path,
        "module": str(meta.get("module") or ""),
        "md5":    str(meta.get("md5") or ""),
        "crc32":  str(meta.get("crc32") or ""),
    }


def _discover() -> list[dict[str, str]]:
    """Every IDA answering on the range.

    Scanning, not a registry file: a file left behind by a crashed IDA lies
    about what is running, a socket cannot.
    """
    # Two stages. A bare TCP connect tells "is anything there" in a millisecond
    # on loopback; only ports that accept get the get_metadata round trip. Doing
    # the HTTP call blind cost 1.5 s per silent port — fifteen seconds to learn
    # that nothing is running.
    open_ports: list[int] = []
    for p in range(BASE_PORT, BASE_PORT + PORT_RANGE):
        try:
            with socket.create_connection((HOST, p), timeout=0.15):
                open_ports.append(p)
        except OSError:
            continue
    found: list[dict[str, str]] = []
    for p in open_ports:
        ident = _identity(p)
        if ident:
            found.append(ident)
    return found


def _fmt(sessions: list[dict[str, str]]) -> str:
    return ", ".join(f"{s['port']}={s['file'] or '?'}" for s in sessions)


# ---------------------------------------------------------------------------
# selection
# ---------------------------------------------------------------------------
_selected: int | None = None

_NO_IDA = (f"no IDA answering on {HOST}:{BASE_PORT}-{BASE_PORT + PORT_RANGE - 1} "
           "— open a database and start the MCP server (Ctrl-Alt-M)")


def _resolve(target: str) -> dict[str, str]:
    """A port number, an exact file name, or a case-insensitive substring of it."""
    sessions = _discover()
    if not sessions:
        raise RuntimeError(_NO_IDA)
    t = str(target).strip()
    if t.isdigit():
        for s in sessions:
            if s["port"] == t:
                return s
        raise RuntimeError(f"no IDA on port {t}; running: {_fmt(sessions)}")
    tl = t.lower()
    exact = [s for s in sessions if s["file"].lower() == tl]
    if len(exact) == 1:
        return exact[0]
    partial = [s for s in sessions
               if tl in s["file"].lower() or tl in s["path"].lower()]
    if len(partial) == 1:
        return partial[0]
    if len(partial) > 1:
        raise RuntimeError(f"'{t}' is ambiguous: {_fmt(partial)}")
    raise RuntimeError(f"no IDA has '{t}' open; running: {_fmt(sessions)}")


def _current_port() -> int:
    """The selected IDA, or the only one if nothing was chosen.

    Two IDAs and no choice is an error, not a coin flip — silently picking the
    first is exactly the behaviour this server exists to remove.
    """
    global _selected
    if _selected is not None:
        return _selected
    sessions = _discover()
    if not sessions:
        raise RuntimeError(_NO_IDA)
    if len(sessions) > 1:
        raise RuntimeError("several IDAs are running and none is selected — call "
                           f"use_ida_session with one of: {_fmt(sessions)}")
    _selected = int(sessions[0]["port"])
    return _selected


# ---------------------------------------------------------------------------
# the hub's own tools
# ---------------------------------------------------------------------------
@mcp.tool()
def list_ida_sessions() -> list[dict[str, str]]:
    """Every running IDA with the MCP plugin started: port, file, path, hashes.

    Pick one with use_ida_session. With exactly one running nothing needs
    choosing; with several, every other tool refuses until you choose, rather
    than silently talking to whichever started first.
    """
    return _discover()


@mcp.tool()
def use_ida_session(target: str) -> str:
    """Point every later call at one IDA.

    target: a port ('13338'), an exact file name ('SonicIntro.gen'), or a
    unique substring of the file name or path ('sonic').
    """
    global _selected
    s = _resolve(target)
    _selected = int(s["port"])
    return f"using IDA on port {s['port']}: {s['file']}"


@mcp.tool()
def current_ida_session() -> dict[str, str]:
    """Which IDA the forwarded tools go to right now."""
    port = _current_port()
    return _identity(port) or {"port": str(port), "file": "?"}


# ---------------------------------------------------------------------------
# transparent forwarding of the plugin's own tools
#
# The list comes from a live IDA at startup, so a plugin upgrade that adds
# tools needs no change here. Each is registered under the plugin's own name,
# description and input schema, and calls through to whichever IDA is selected
# at CALL time — so switching sessions mid-conversation just works.
# ---------------------------------------------------------------------------
class _ForwardedTool(Tool):
    """A real FastMCP Tool whose schema is the plugin's and whose body is a
    forward.

    Subclassing rather than imitating: FastMCP lists tools by reading pydantic
    fields, so an ad-hoc object with the right attribute names would work today
    and break on the next mcp release. Only run() is replaced — the default
    validates arguments against a signature this forwarder does not have.
    """

    async def run(self, arguments: dict[str, Any], context: Any = None,
                  convert_result: bool = False) -> Any:
        return _call_tool(_current_port(), self.name, arguments or {})


async def _forward_placeholder(**kwargs: Any) -> Any:   # never called: run() is overridden
    raise RuntimeError("forwarded tool invoked without run()")


def _register_plugin_tools() -> int:
    sessions = _discover()
    if not sessions:
        return 0
    try:
        listing = _rpc(int(sessions[0]["port"]), "tools/list", {})
    except Exception:
        return 0
    tools = listing.get("tools", []) if isinstance(listing, dict) else []
    own = set(mcp._tool_manager._tools)                       # noqa: SLF001
    n = 0
    for t in tools:
        name = t.get("name")
        if not name or name in own:
            continue
        # FastMCP derives a schema from the function signature; a forwarder has
        # none. Register the plugin's own schema so the agent sees the real
        # parameters.
        base = Tool.from_function(_forward_placeholder, name=name,
                                  description=t.get("description", ""))
        # model_dump() drops `fn` (a callable is not serialisable), so build the
        # subclass from the validated fields directly rather than round-tripping.
        fwd = _ForwardedTool.model_construct(**{k: getattr(base, k) for k in Tool.model_fields})
        fwd.parameters = t.get("inputSchema") or {"type": "object", "properties": {}}
        mcp._tool_manager._tools[name] = fwd                    # noqa: SLF001
        n += 1
    return n


if __name__ == "__main__":
    n = _register_plugin_tools()
    if n == 0:
        # Not fatal — the session tools still work — but a hub with three tools
        # looks broken, so say why.
        print("ida-hub: no IDA answering yet; only the session tools are available "
              "until one is running (start the MCP server in IDA with Ctrl-Alt-M)",
              file=sys.stderr)
    else:
        print(f"ida-hub: forwarding {n} tools from the ida-pro-mcp plugin", file=sys.stderr)
    mcp.run()
