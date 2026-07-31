"""MCP server for the SMD DGX debugger.

Talks to the control socket opened by the IDA plugin (or any other host that
runs a BridgeServer), so an agent drives the *same* emulator session the user
is looking at, not a private copy.

The C++ side speaks a deliberately dumb line protocol; everything structured
lives here. Start the emulator in IDA first ("start process"), then run this.

    pip install mcp
    python smd_dgx_mcp.py
"""

from __future__ import annotations

import base64
import os
import socket
import struct
import threading
import zlib

from mcp.server.fastmcp import FastMCP, Image

HOST = os.environ.get("SMD_DGX_HOST", "127.0.0.1")

# A pinned port, or None to discover one. Several emulators can run at once, so
# a hardcoded port either reaches the wrong game or nothing at all.
_env_port = os.environ.get("SMD_DGX_PORT")
PORT: int | None = int(_env_port) if _env_port else None

BASE_PORT = 27042
PORT_RANGE = 16


def _ping(port: int, timeout: float = 0.25) -> dict[str, str] | None:
    """Ask one port who it is. None if nothing of ours answers."""
    try:
        with socket.create_connection((HOST, port), timeout=timeout) as s:
            s.sendall(b"ping" + bytes([10]))
            reply = s.recv(512).decode(errors="replace").split(chr(10), 1)[0]
    except OSError:
        return None
    if not reply.startswith("ok smd_dgx"):
        return None
    info = {"port": str(port)}
    for tok in reply.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            info[k] = v
    return info


def _discover() -> list[dict[str, str]]:
    """Every emulator answering on the loopback range.

    Scanning rather than reading a file of published ports: a file left behind
    by a crashed session lies about what is running, a socket cannot.
    """
    out = []
    for p in range(BASE_PORT, BASE_PORT + PORT_RANGE):
        info = _ping(p)
        if info:
            out.append(info)
    return out

mcp = FastMCP("smd-dgx")


class Bridge:
    """One reconnecting line-protocol connection, serialised by a lock.

    The socket serves a single client at a time, so concurrent tool calls must
    not interleave on it.
    """

    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._buf = b""
        self._lock = threading.Lock()
        self._port: int | None = None      # discovered once, then sticky

    def __init_port(self) -> int:
        if PORT is not None:
            return PORT
        if self._port is not None:
            return self._port
        found = _discover()
        if not found:
            raise RuntimeError(
                f"no SMD DGX emulator answering on {HOST}:"
                f"{BASE_PORT}-{BASE_PORT + PORT_RANGE - 1} — start the game first"
            )
        self._port = int(found[0]["port"])
        return self._port

    def reconnect(self, port: int) -> None:
        """Switch to a different emulator, explicitly."""
        with self._lock:
            self._drop()
            self._port = int(port)

    def port(self) -> int | None:
        return self._port if PORT is None else PORT

    def _connect(self) -> socket.socket:
        if self._sock is None:
            port = self.__init_port()
            s = socket.create_connection((HOST, port), timeout=10)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._sock, self._buf = s, b""
        return self._sock

    def _drop(self, forget_port: bool = False) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock, self._buf = None, b""
        # The silent retry reconnects to the SAME port on purpose. Rediscovering
        # here would let a reconnect land on a different game mid-session, and
        # every later answer would be about something the caller never asked
        # for. Only an explicit reconnect() may change target.
        if forget_port:
            self._port = None

    def command(self, line: str) -> str:
        """Send one command, return the reply without its 'ok ' prefix.

        Raises RuntimeError on an 'err ...' reply or a dead connection.
        """
        with self._lock:
            for attempt in (1, 2):  # one silent retry: IDA may have restarted
                try:
                    sock = self._connect()
                    sock.sendall(line.encode() + b"\n")
                    while b"\n" not in self._buf:
                        chunk = sock.recv(65536)
                        if not chunk:
                            raise ConnectionError("bridge closed")
                        self._buf += chunk
                    raw, self._buf = self._buf.split(b"\n", 1)
                    break
                except (OSError, ConnectionError):
                    self._drop()
                    if attempt == 2:
                        raise RuntimeError(
                            f"cannot reach the SMD DGX bridge at {HOST}:"
                            f"{self._port or PORT} — is the emulator running?"
                        )

        reply = raw.decode(errors="replace").strip()
        if reply.startswith("err"):
            raise RuntimeError(reply[3:].strip() or "command failed")
        return reply[2:].strip() if reply.startswith("ok") else reply


bridge = Bridge()


def _kv(reply: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for tok in reply.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def _png(rgb565: bytes, w: int, h: int) -> bytes:
    """Encode RGB565 as PNG using only the stdlib."""
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # filter type: none
        row = rgb565[y * w * 2 : (y + 1) * w * 2]
        for x in range(w):
            p = row[x * 2] | (row[x * 2 + 1] << 8)
            r, g, b = (p >> 11) & 0x1F, (p >> 5) & 0x3F, p & 0x1F
            # widen 5/6-bit channels so full-scale stays full-scale
            rows += bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))


def _hex(value: str) -> str:
    """Normalise a hex address for the wire.

    Not str.lstrip('0x'): that strips *characters*, so "0" and "0x0" both become
    the empty string, the bridge sees one argument fewer than it expects, and
    every later argument shifts by one. Address zero is not an exotic input —
    it is the top of the 68000 vector table.
    """
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    s = s.lstrip("0")
    return s or "0"


# ---------------------------------------------------------------------------
# sessions
# ---------------------------------------------------------------------------
@mcp.tool()
def list_sessions() -> list[dict[str, str]]:
    """Every emulator currently running, with the game each one has loaded.

    Returns port, pid, crc (the ROM's calculated checksum), serial and name.
    Use connect_session to pick one; without a choice the first is used.
    """
    return _discover()


@mcp.tool()
def connect_session(port: int) -> str:
    """Point every later tool call at the emulator on this port.

    Only needed when more than one is running — see list_sessions.
    """
    info = _ping(int(port))
    if info is None:
        raise RuntimeError(f"nothing answering on {HOST}:{port}")
    bridge.reconnect(int(port))
    name = info.get("name", "-")
    return f"connected to port {port} (pid {info.get('pid', '?')}, {name})"


@mcp.tool()
def current_session() -> dict[str, str]:
    """Which emulator these tools are talking to right now."""
    bridge.command("ping")          # forces a connection if there is not one
    port = bridge.port()
    info = _ping(port) if port else None
    return info or {"port": str(port or 0)}


# ---------------------------------------------------------------------------
# state
# ---------------------------------------------------------------------------
@mcp.tool()
def status() -> str:
    """Whether the emulator is running, whether it is paused, and the current PC."""
    kv = _kv(bridge.command("status"))
    state = "paused" if kv.get("paused") == "1" else "running"
    if kv.get("running") != "1":
        state = "not running"
    return f"{state}, pc={kv.get('pc', '?')}"


@mcp.tool()
def get_registers(cpu: str = "m68k") -> dict[str, str]:
    """Read CPU registers. cpu is 'm68k' (default) or 'z80'. Values are hex."""
    return _kv(bridge.command("regsz80" if cpu.lower() in ("z80", "z-80") else "regs68k"))


@mcp.tool()
def get_vdp_state() -> dict[str, str]:
    """Read VDP registers (reg = 24 bytes hex), status, and DMA length/source/type."""
    return _kv(bridge.command("vdp"))


# ---------------------------------------------------------------------------
# memory
# ---------------------------------------------------------------------------
@mcp.tool()
def read_memory(address: str, size: int) -> str:
    """Read from the 68k bus. address is hex (e.g. 'FF0000'); returns hex bytes."""
    return bridge.command(f"read {_hex(address)} {size}")


@mcp.tool()
def write_memory(address: str, data_hex: str) -> str:
    """Write hex bytes to the 68k bus. address is hex."""
    bridge.command(f"write {_hex(address)} {data_hex}")
    return "written"


@mcp.tool()
def list_regions() -> list[dict[str, str]]:
    """List memory regions (ROM, RAM 68K, RAM Z80, VRAM, CRAM, VSRAM, register banks)."""
    out = []
    for tok in bridge.command("regions").split():
        parts = tok.split(":")
        if len(parts) == 5:
            out.append({"id": parts[0], "name": parts[1], "base": parts[2],
                        "size": parts[3], "writable": parts[4]})
    return out


@mcp.tool()
def read_region(region_id: int, offset: str = "0", size: int = 256) -> str:
    """Read from a region by id (see list_regions). offset is hex; returns hex bytes."""
    return bridge.command(f"readregion {region_id} {_hex(offset)} {size}")


@mcp.tool()
def write_region(region_id: int, offset: str, data_hex: str) -> str:
    """Write hex bytes into a region by id. offset is hex.

    This is how VRAM, CRAM and VSRAM are edited: they are not on the 68000 bus,
    so write_memory cannot reach them.
    """
    bridge.command(f"writeregion {region_id} {_hex(offset)} {data_hex}")
    return "written"


@mcp.tool()
def set_vdp_register(index: int, value: str) -> str:
    """Set one VDP register (0-23) to a hex byte, in the shadow the core reads.

    Changes take effect as the renderer next consults the register, exactly as
    the Gens debugger did — it does not replay the write through the VDP port.
    """
    bridge.command(f"setvdpreg {int(index)} {_hex(value)}")
    return f"reg {index} = {value}"


# ---------------------------------------------------------------------------
# run control
# ---------------------------------------------------------------------------
@mcp.tool()
def pause() -> str:
    """Pause the emulator at the next instruction."""
    bridge.command("pause")
    return status()


@mcp.tool()
def resume() -> str:
    """Resume execution."""
    bridge.command("resume")
    return "resumed"


@mcp.tool()
def step(over: bool = False, cpu: str = "m68k") -> str:
    """Execute one instruction. over=True steps over calls. cpu: 'm68k' or 'z80'.

    The Z80 needs its own step: it retires far more instructions per frame than
    the 68000, so a step aimed at the wrong CPU is consumed by the other one.
    """
    suffix = " z80" if cpu.lower().startswith("z") else ""
    bridge.command(("stepo" if over else "stepi") + suffix)
    return status()


@mcp.tool()
def add_breakpoint(address: str, kind: str = "x", end: str | None = None,
                   cpu: str = "m68k", vdp: bool = False) -> str:
    """Add a breakpoint and return its id.

    kind: 'x' execute, 'r' read, 'w' write. Addresses are hex, end defaults to
    address. cpu: 'm68k' or 'z80' — both CPUs share one numeric address space,
    so an unlabelled breakpoint matches the wrong code. vdp=True puts the
    address in VDP space instead of the bus: VRAM at 0, CRAM at +0x10000,
    VSRAM at +0x20000.

    The returned id is what a stop event reports, so keep it to know which of
    several breakpoints fired.
    """
    a = _hex(address)
    c = "z80" if cpu.lower().startswith("z") else "m68k"
    return bridge.command(
        f"bpadd {kind} {a} {_hex(end or address)} cpu={c} vdp={1 if vdp else 0}")


@mcp.tool()
def clear_breakpoints() -> str:
    """Remove every breakpoint."""
    bridge.command("bpclear")
    return "cleared"


# ---------------------------------------------------------------------------
# the loop
#
# Set a breakpoint, resume, wait. Without wait_for_stop an agent has to poll,
# which either burns a core or adds enough latency that "which frame did that
# happen on?" stops being answerable. Without frame_advance it cannot time an
# input at all: while the machine is being debugged it is not bound to the wall
# clock, so sleeping proves nothing.
# ---------------------------------------------------------------------------
@mcp.tool()
def wait_for_stop(timeout_ms: int = 10000) -> dict[str, str]:
    """Block until the emulator stops, and report why.

    Returns reason ('paused' or 'stopped'), pc, cpu, and bp — the id of the
    breakpoint that fired, or -1 when the stop was a step, an explicit pause or
    a finished frame_advance. Returns reason='timeout' if nothing stopped in
    time; that is an answer, not an error.
    """
    reply = bridge.command(f"wait {int(timeout_ms)}")
    if reply.startswith("timeout"):
        return {"reason": "timeout"}
    parts = reply.split()
    out = {"reason": parts[0] if parts else "?"}
    out.update(_kv(reply))
    return out


@mcp.tool()
def frame_advance(frames: int = 1) -> dict[str, str]:
    """Run exactly this many frames, then stop. Resumes first if paused.

    This is how an input is timed: hold buttons with set_buttons, advance the
    frames you want them held for, then release.
    """
    bridge.command(f"frameadv {int(frames)}")
    return wait_for_stop(30000)


@mcp.tool()
def search_memory(region_id: int, pattern_hex: str, max_hits: int = 256) -> list[str]:
    """Find every offset in a region where these bytes appear. Offsets are hex.

    Done on the emulator side: the alternative is hauling the whole region
    across as ASCII hex for every attempt.
    """
    reply = bridge.command(f"search {region_id} {pattern_hex} {int(max_hits)}")
    return reply.split() if reply else []


@mcp.tool()
def snapshot_region(region_id: int) -> str:
    """Remember a region's contents so diff_region can say what changed later."""
    return "snapshot: " + bridge.command(f"snap {region_id}") + " bytes"


@mcp.tool()
def diff_region(region_id: int, max_hits: int = 256) -> list[dict[str, str]]:
    """What changed in a region since snapshot_region, as offset/old/new (hex).

    The classic way to find a variable: snapshot, play, diff, repeat until one
    address is left. Snapshot first or this reports an error.
    """
    reply = bridge.command(f"diff {region_id} {int(max_hits)}")
    out = []
    for tok in reply.split():
        p = tok.split(":")
        if len(p) == 3:
            out.append({"offset": p[0], "old": p[1], "new": p[2]})
    return out


@mcp.tool()
def poll_events(max_events: int = 32) -> list[dict[str, str]]:
    """Drain queued events without blocking: seq, type, pc, cpu, bp.

    Prefer wait_for_stop when you are waiting for something; use this to see
    what happened while you were busy. A 'dropped' entry means the queue
    overflowed and events were lost.
    """
    reply = bridge.command(f"events {int(max_events)}")
    parts = reply.split()
    out = []
    for tok in parts[1:] if parts else []:
        f = tok.split(",")
        if len(f) >= 4:
            out.append({"seq": f[0], "type": f[1], "pc": f[2], "cpu": f[3],
                        "bp": f[4] if len(f) > 4 else "-1"})
    return out


@mcp.tool()
def get_callstack(cpu: str = "m68k") -> list[str]:
    """Return addresses of the calls currently on the stack, outermost first."""
    suffix = " z80" if cpu.lower().startswith("z") else ""
    reply = bridge.command("callstack" + suffix)
    return reply.split() if reply else []


@mcp.tool()
def read_z80_memory(address: str = "0", size: int = 256) -> str:
    """Read the Z80's own 16-bit space. $0000-$1FFF is sound RAM, $8000+ is the
    68000 window; the YM2612 and VDP ports read as FF because sampling them
    would change hardware state behind the driver's back."""
    return bridge.command(f"readz80 {_hex(address)} {int(size)}")


@mcp.tool()
def write_registers(registers: dict[str, str], cpu: str = "m68k") -> str:
    """Write CPU registers: a name -> hex value mapping. cpu: 'm68k' or 'z80'.

    m68k: d0-d7, a0-a7, pc, sr, usp, isp.  z80: af, bc, de, hl, af2, bc2, de2,
    hl2, ix, iy, sp, pc, i, r, im, iff1, iff2, halt.

    An explicit mapping rather than keyword arguments, so the tool schema names
    one parameter instead of depending on how the host expands **kwargs.
    """
    regs = registers or {}
    if not regs:
        return "nothing to write"
    cmd = "wregz80" if cpu.lower().startswith("z") else "wreg68k"
    pairs = " ".join(f"{k}={_hex(v)}" for k, v in regs.items())
    bridge.command(f"{cmd} {pairs}")
    return f"wrote {len(regs)} register(s)"


@mcp.tool()
def get_sound_state() -> dict[str, str]:
    """Raw YM2612 (fm0/fm1, 256 bytes hex each) and PSG registers."""
    return _kv(bridge.command("sound"))


@mcp.tool()
def list_breakpoints() -> list[dict[str, str]]:
    """List breakpoints: id, kind (x/r/w), start, end, cpu, vdp, enabled."""
    out = []
    for tok in bridge.command("bplist").split():
        p = tok.split(",")
        # The server sends seven fields; requiring exactly four made this
        # return an empty list for every breakpoint that has ever existed.
        # Accept four so an older host still works, and fill the rest in.
        if len(p) < 4:
            continue
        out.append({
            "id": p[0], "kind": p[1], "start": p[2], "end": p[3],
            "cpu":     p[4] if len(p) > 4 else "m68k",
            "vdp":     p[5] if len(p) > 5 else "0",
            "enabled": p[6] if len(p) > 6 else "1",
        })
    return out


@mcp.tool()
def remove_breakpoint(bp_id: int) -> str:
    """Remove a breakpoint by id."""
    bridge.command(f"bpdel {bp_id}")
    return "removed"


# ---------------------------------------------------------------------------
# interaction
# ---------------------------------------------------------------------------
BUTTONS = {"up": 0x001, "down": 0x002, "left": 0x004, "right": 0x008,
           "b": 0x010, "c": 0x020, "a": 0x040, "start": 0x080,
           "z": 0x100, "y": 0x200, "x": 0x400, "mode": 0x800}


@mcp.tool()
def set_buttons(buttons: list[str]) -> str:
    """Hold exactly these controller buttons (empty list releases everything).

    Names: up, down, left, right, a, b, c, x, y, z, start, mode.
    The pad stays in this state until changed, so press then release to tap.
    """
    mask = 0
    for name in buttons:
        key = name.strip().lower()
        if key not in BUTTONS:
            raise ValueError(f"unknown button {name!r}; valid: {', '.join(BUTTONS)}")
        mask |= BUTTONS[key]
    bridge.command(f"pad {mask:x}")
    return f"pad = {sorted(b.lower() for b in buttons) or 'released'}"


@mcp.tool()
def screenshot() -> Image:
    """Capture what is on screen right now, as a PNG."""
    reply = bridge.command("frame")
    w_s, h_s, b64 = reply.split(" ", 2)
    w, h = int(w_s), int(h_s)
    return Image(data=_png(base64.b64decode(b64), w, h), format="png")


# ---------------------------------------------------------------------------
# save states
# ---------------------------------------------------------------------------
@mcp.tool()
def save_state(path: str) -> str:
    """Save a save state to an absolute path. Use it to bookmark a scene."""
    bridge.command(f"savestate {path}")
    return f"saved to {path}"


@mcp.tool()
def load_state(path: str) -> str:
    """Restore a save state previously written by save_state."""
    bridge.command(f"loadstate {path}")
    return f"loaded {path}"


if __name__ == "__main__":
    mcp.run()
