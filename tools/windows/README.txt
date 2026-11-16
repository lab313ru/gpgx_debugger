SMD DGX — standalone debugger
=============================

Sega Mega Drive / Genesis debugger built on Genesis Plus GX.
Windows x64, self-contained: run smd_dgx_qt6.exe, nothing to install.

  @VERSION@ / @DATE@
  https://github.com/lab313ru/gpgx_debugger  (branch debugging)

  smd_dgx_qt6.exe [rom]     .bin / .gen / .md / .smd


CONTROLS
--------

  Arrows            D-pad
  Enter             Start
  Backspace         Mode
  Z X C             A B C
  A S D             X Y Z

Letter keys are matched by physical position, so they stay where they are
on a non-QWERTY layout. Click the screen to give it focus. Right-click it
for pixel-perfect / smoothing / aspect options.

  F5  pause      F9  resume
  F7  step into  F8  step over  Ctrl+F7  step out

The steps act on the CPU whose tab is in front in the Debugger window, and
the Debug menu says "(Z80)" when it is the Z80.


DEBUGGER  (View -> Debugger)
----------------------------

A 68000 tab and a Z80 tab, each with:

  - a disassembly listing around that CPU's PC; Go to; F2 or a double-click
    on a line toggles a breakpoint there;
  - the registers, editable while paused (flags as checkboxes);
  - Step Into / Step Over / Step Out / Run / Pause buttons.

The Z80 tab also shows IM, IFF1/IFF2, HALT, the 68000 range its bank window
maps, and whether the 68000 holds the Z80 in reset or has taken its bus: a
Z80 step only finishes once the Z80 gets to run. A Z80 stop brings its tab
forward.

Beside the tabs, the breakpoint panel: PC (68000 or Z80), 68K RAM and Z80
RAM reads and writes, VRAM / CRAM / VSRAM. It lists every breakpoint,
including those set from IDA or an MCP agent.


OPTIONS AND RESET
-----------------

Options -> System / Video / Audio / Input / Emulation Hacks holds the
Genesis Plus GX core options: region, console model, TMSS, lock-on
cartridges, FM chip, filters, overscan and the rest. Most apply at once;
a few take effect at File -> Hard Reset or Reload ROM, and their tooltip
says which.


VIEWS  (View menu — all dockable)
---------------------------------

  Debugger         see above
  VDP Ram          palette grid and tile browser; dump/load VRAM and CRAM,
                   export a YY-CHR palette
  VDP Registers    every mode bit as a checkbox, editable; changed
                   registers turn red
  VDP Sprites      the sprite table in link-chain or flat order; renders
                   the selected sprite under all four palette lines
  Plane Explorer   planes A/B, Window, sprites; zoom x1-x8 (Ctrl+wheel),
                   middle-drag to pan, click to pin a tile
  Scroll           per-scanline H and per-column V scroll — how most
                   raster effects on this hardware are built
  Hex Editor       all nine regions, editable, with Go To and dump
  RAM Search       classic value search over 68K work RAM
  RAM Watch        reads and writes Gens .wch files; double-click a value
                   to poke it
  YM2612 & PSG     per-channel operator registers, ADSR sketch, timers
  Save States      named and grouped, stored beside the ROM

Zoom levels, the pinned tile, dock layout, window geometry, the core
options and the watch list survive a restart. Settings live in
%LOCALAPPDATA%\smd_dgx\views.ini


IDA AND AGENTS
--------------

The control socket opens once a ROM is loaded, so MCP tools and IDA can drive
this app the same way they drive the plugin. In IDA, Debugger -> Attach to
process... lists this app by its ROM file name; the status bar here shows the
port.


LICENCE
-------

Genesis Plus GX licence (non-commercial) — see LICENSE.txt in the repo.
The 68000 disassembler is Musashi's m68kdasm.c by Karl Stenerud (MIT).
Genesis Plus GX by Charles MacDonald and Eke-Eke.
Gens debug tooling this follows by DrMefistO (lab313ru).
