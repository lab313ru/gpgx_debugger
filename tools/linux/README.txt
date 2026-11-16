SMD DGX - standalone debugger for Linux
=======================================

Sega Mega Drive / Genesis debugger built on Genesis Plus GX.
Linux x86_64. Qt @QT_VERSION@ and SDL2 come with it: nothing to install.

  @VERSION@
  https://github.com/DramaDesu/gpgx_debugger


RUN
---

The same build comes two ways.

  smd_dgx-linux-x86_64.AppImage [rom]      one file; chmod +x it first

    It mounts itself with FUSE, so it needs /dev/fuse and a fusermount or
    fusermount3 program (on Ubuntu, the fuse3 package; libfuse2 is not
    needed). Without FUSE, run it as

      APPIMAGE_EXTRACT_AND_RUN=1 ./smd_dgx-linux-x86_64.AppImage [rom]

    which unpacks it to /tmp on every start, or use the folder below.

  smd_dgx_standalone_linux_x86_64/AppRun [rom]

    The tarball: the same files, already unpacked. Keep the folder whole;
    AppRun finds everything relative to itself.

  ROMs: .bin / .gen / .md / .smd


REQUIREMENTS
------------

  glibc @GLIBC@ or newer, and a libstdc++ with GLIBCXX_@GLIBCXX@ or newer.
  Measured over every binary in the package. For comparison: Ubuntu 22.04
  has glibc 2.35, Debian 12 has 2.36, Ubuntu 24.04 has 2.39.

  An X11 desktop, or Wayland with XWayland: Qt's xcb platform plugin is in
  the package, the Wayland one is not. On Wayland Qt first says it cannot
  find the "wayland" plugin, then opens the window through XWayland.

  Like any AppImage, it takes the libraries every desktop has from the
  system: libstdc++, X11/xcb, OpenGL/EGL, fontconfig, freetype, harfbuzz,
  ALSA (libasound) and zlib. Everything else is in usr/lib.

  Sound: the bundled SDL2 plays through the system's PulseAudio/PipeWire or
  ALSA. If none answers, the emulator runs silently. DGX_NO_AUDIO=1 silences
  it on purpose.


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

  F5  pause    F9  resume    F7  step into    F8  step over    Ctrl+F7  step out


VIEWS  (View menu - all dockable)
---------------------------------

  VDP Ram          palette grid and tile browser; dump/load VRAM and CRAM,
                   export a YY-CHR palette
  VDP Registers    every mode bit as a checkbox, editable; changed
                   registers turn red
  VDP Sprites      the sprite table in link-chain or flat order; renders
                   the selected sprite under all four palette lines
  Plane Explorer   planes A/B, Window, sprites; zoom x1-x8 (Ctrl+wheel),
                   middle-drag to pan, click to pin a tile
  Scroll           per-scanline H and per-column V scroll - how most
                   raster effects on this hardware are built
  Hex Editor       all nine regions, editable, with Go To and dump
  RAM Search       classic value search over 68K work RAM
  RAM Watch        reads and writes Gens .wch files; double-click a value
                   to poke it
  YM2612 & PSG     per-channel operator registers, ADSR sketch, timers
  Save States      named and grouped, stored beside the ROM

Zoom levels, the pinned tile, dock layout, window geometry and the watch
list survive a restart. Settings live in ~/.config/smd_dgx/views.ini
(or $XDG_CONFIG_HOME/smd_dgx/views.ini).


NOTE
----

There is no breakpoint UI in this app - breakpoints are for the IDA plugin
or the MCP interface. Stepping here is 68000 only.

The control socket (127.0.0.1, a free port from 27042 up) opens once a ROM
is loaded, so the MCP server and an IDA can drive this app the same way they
drive the plugin. The status bar shows the port.


LICENCE
-------

Genesis Plus GX licence (non-commercial) - see LICENSE.txt.
Genesis Plus GX by Charles MacDonald and Eke-Eke.
Gens debug tooling this follows by DrMefistO (lab313ru).

usr/lib holds unmodified builds of Qt (LGPL-3), SDL2 (zlib licence) and the
libraries they need, taken from the build machine's distribution, each under
its own licence; their copyright files are in usr/share/doc.
