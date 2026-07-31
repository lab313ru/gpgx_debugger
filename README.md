# SMD DGX

A Sega Mega Drive / Genesis **reverse-engineering toolkit** built on
[Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX): an accurate
emulator you can stop, inspect, script and drive from IDA.

It is a port of the Gens-based tooling from
[smd_ida_tools2](https://github.com/lab313ru/smd_ida_tools2) onto a modern,
cross-platform stack — Genesis Plus GX instead of Gens, Qt6 instead of Win32,
CMake instead of Visual Studio projects, and a plain socket instead of
protobuf/gRPC.

Three hosts share one debug layer:

| Host | What it is |
|---|---|
| `smd_dgx_qt6` | Standalone debugger app — the emulator plus eleven debug views |
| `smd_dgx_ida` | IDA 9.2+ plugin. Runs the emulator **inside IDA's process**, on its own thread |
| `mcp/smd_dgx_mcp.py` | MCP server, so an AI agent drives the same session you are looking at |

The debug layer itself (`smd_dgx_cmake/debugger/`) contains no Qt, no IDA and no
SDL. Every view depends only on `IDebugBackend.h` and `DebugState.h`, which is
why the same widgets run in the standalone app and docked inside IDA.

---

## The standalone app

`smd_dgx_qt6 [rom]` — arrows are the D-pad, `Z/X/C` and `A/S/D` are the six face
buttons, Enter is Start. Letter keys are matched by physical position, so they
stay put on a non-QWERTY layout. `F5`/`F9` pause and resume, `F7`/`F8` step into
and over.

Views, all dockable and toggled from the **View** menu:

- **VDP Ram** — palette grid and tile browser. Click a palette cell to pick the
  decode colour, click a tile for a zoomed preview with CRAM/RGB readouts.
  Dump/load VRAM and CRAM as plain binary, export a YY-CHR palette.
- **VDP Registers** — every mode bit as a labelled checkbox, decoded fields as
  editable hex. Registers that changed since the last refresh turn red.
- **VDP Sprites** — the sprite attribute table, in link-chain order (with loop
  detection) or flat. Select a row to render that sprite under all four palette
  lines. Dumps to text.
- **Plane Explorer** — planes A/B, Window and the sprite layer. Zoom ×1–×8
  (Ctrl+wheel keeps the centre), middle-drag to pan, click to pin a tile.
- **Scroll** — per-scanline horizontal and per-column vertical scroll, which is
  how most raster effects on this hardware are built. No Gens equivalent.
- **Hex Editor** — all nine memory regions (ROM, 68K RAM, Z80 RAM, VRAM, CRAM,
  VSRAM, register banks), editable, with selection, clipboard, Go To and dump.
- **RAM Search** and **RAM Watch** — the classic pair. Watch lists read and
  write Gens `.wch` files, and double-clicking a value pokes it.
- **YM2612 & PSG** — per-channel operator registers, an ADSR sketch, LFO, DAC,
  timers, decoded key-on state. Read-only.
- **Save States** — named and grouped, stored beside the ROM.

Zoom levels, the pinned tile, dock layout, window geometry and the watch list
all survive a restart.

## The IDA plugin

Open a Mega Drive ROM, choose **SMD DGX** as the debugger, `F9`. The emulator
runs inside IDA, so IDA's own windows operate on the live machine:

- **Registers** — 68000 (named SR flags, plus DMA length/source and the address
  the next VDP write will land at) and a separate VDP register class. Editable,
  except PC.
- **Memory** — ROM, work RAM, Z80 RAM, and the VDP memories as pseudo-segments
  at `$D00000` (VRAM), `$D10000` (CRAM), `$D20000` (VSRAM). Reads are
  side-effect free; they never touch an IO handler.
- **Breakpoints** — execute, read, write and read+write, ranged, on **both**
  CPUs.
- **Stepping**, and a code map that marks everything the emulator actually
  executed as code — the old Gensida trick for ROMs static analysis cannot
  untangle.
- Eleven of the views above, dockable inside IDA, plus five layout presets.
- **ROM patching** — push IDA's patched bytes into the running emulator, revert
  them, or export the current ROM image.
- **`J` on an instruction** decodes Mega Drive magic numbers into a comment:
  VDP access modes, register writes, Z80 bus control, SR masks. A-line and
  F-line traps are decoded as instructions with an xref into their handler.

Two IDA loaders are included: one for Mega Drive ROMs (segments, header fields,
vector table) and one for dumped Z80 sound drivers.

### Debugging the Z80 alongside the 68000

An IDA database holds exactly one processor module, so a game and its sound
driver cannot share one. They share an *emulator* instead:

```
 IDA #1 (68000)                        IDA #2 (Z80)
 runs the emulator  ──── socket ────►  attaches to it
```

1. In the 68000 database, start the game and let it reach sound init.
2. **Debugger → SMD patches → Dump Z80 driver...** writes the live 8 KB of
   sound RAM. Drivers ship compressed or relocated, so the copy sitting in the
   ROM is usually not what the Z80 executes.
3. Open that dump in a second IDA with the Z80 driver loader, choose
   **SMD DGX Z80**, and start — it attaches instead of launching anything.

Any stop, including one caused by a 68000 breakpoint in the other database, is
reported in the Z80 database at the Z80's own PC. Detaching leaves the game
running. Z80 breakpoints carry their CPU, so the same numeric address in the two
databases never collides.

## Driving it from an agent

The emulator serves a line protocol on loopback — one command per line, one
reply per line, no JSON and no dependencies on the C++ side.
`mcp/smd_dgx_mcp.py` puts an MCP server on top, so an agent works on the *same*
session you have open rather than a private copy.

Beyond reading memory and registers, it has the primitives a reverse-engineering
loop actually needs:

- `wait_for_stop` — block until the machine stops, and be told **which**
  breakpoint did it. Polling substitutes for this only by burning a core.
- `frame_advance` — run exactly N frames. There is no other way to time an
  input: under a debugger the machine is not bound to the wall clock, so
  sleeping proves nothing.
- `search_memory`, `snapshot_region` / `diff_region` — find a value, then find
  what moved. Emulator-side, so a RAM search is not 64 KB of hex per round.
- `screenshot` — the framebuffer as a PNG, so a model can *see* the game.

Several emulators can run at once. Each takes a free port from 27042 upward and
identifies itself — pid, ROM checksum, serial, title — so `list_sessions` tells
you whose game is whose. Two games means two processes: the emulator core is
global C state, and two of them cannot share one.

---

## Building

The source root is `smd_dgx_cmake/`, **not** the repository root — the root
`CMakeLists.txt` builds the upstream libretro core and none of this.

SDL2 is required on every platform. Qt6 is optional, but without it the
debugger app is silently skipped. ImGui and OpenGL are optional. zlib is
vendored and built in-tree.

**Windows** — the only path that has actually been run. From an x64 Native
Tools prompt:

```
cd smd_dgx_cmake
cmake --preset x64-release
cmake --build ../builds/x64-release
```

**Linux / macOS** — expected to work, never run:

```
cmake -S smd_dgx_cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Set `VCPKG_ROOT` and the vcpkg toolchain is picked up automatically; leave it
unset and dependencies resolve from the system.

### The IDA modules

Optional, and skipped entirely without an SDK — the normal path if you only
want the standalone app. The SDK is
[open source](https://github.com/HexRaysSA/ida-sdk) and is **not**
redistributed here.

```
cmake --preset x64-release ^
      -DIDA_SDK_DIR=<sdk>/src ^
      -DIDA_INSTALL_DIR="C:/Program Files/IDA Professional 9.3"
```

`IDA_SDK_DIR` is the directory containing `include/ida.hpp`. `IDA_INSTALL_DIR`
is optional and copies the modules into `plugins/` and `loaders/`; if IDA is
running and holding a file open, the copy is skipped with a message rather than
failing the build.

**Build the plugin in Release.** Nothing enforces it, but the SDK ships only
release Qt import libraries and IDA is a release binary — a debug-CRT plugin
sharing Qt objects with a release host is a crash waiting to happen.

### Tests

```
ctest --test-dir <build> --output-on-failure
```

42 cases over the shared debug layer — no Qt, no IDA, no SDL, and no ROM file:
the tests synthesise their own Mega Drive, so nothing is ever skipped for a
missing dump.

---

## Status

Stated honestly, because a README that lists only successes is one you cannot
plan against.

**Verified**, by tests and by hand: run control and stepping on both CPUs;
breakpoints, including data breakpoints with width-aware matching; the
byte-order and region contracts; CRAM packing; save states; the whole bridge
protocol; multi-session port allocation; and every standalone view listed above.

**Built but never driven by hand:**

- The Z80 chain end to end through IDA's own interface. The bridge-level
  behaviour is tested; the IDA UI path is not.
- Any build on Linux or macOS. The reasons it definitely failed there are gone
  — real audio, portable sockets, per-platform IDA module suffixes — but
  nothing has ever been configured there. On Linux the IDA plugin additionally
  needs its static dependencies rebuilt as position-independent code.
- In IDA: VDP-space breakpoints, breakpoint conditions, layout presets, save
  states, ROM patching, the `J` decoder, A/F-line decoding.

**Known broken:** the call-stack window is unreachable (the handler exists, but
the debugger does not advertise `DBG_HAS_UPDATE_CALL_STACK`); `stepOver` does
not check that the emulator is paused; `J` overwrites an existing comment;
`RemoteBackend` reports success unconditionally on some writes; CRAM/VSRAM
breakpoint addresses are not masked the way the core masks them.

`smd_dgx_cmake/ARCHITECTURE.md` carries the design, the contracts that must not
be broken, and the constraints that cost the most to discover.

## Licence

This fork inherits the Genesis Plus GX licence — see `LICENSE.txt`. Note it is
**non-commercial**.

One exception: `smd_dgx_cmake/ida_loader/z80_loader.{cpp,h}` is ported from
smd_ida_tools2 and remains GPL-2-or-later. It builds into a standalone IDA
loader module and is linked into nothing else here.

## Credits

[Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX) by
[Charles MacDonald](http://www.techno-junk.org/) and Eke-Eke — the emulator all
of this is built on, and the reason any of it is accurate. If you find this
useful, the upstream project is the one to support.

The Gens debug windows and the IDA integration this port follows are
DrMefistO's: [lab313ru/smd_ida_tools2](https://github.com/lab313ru/smd_ida_tools2).
