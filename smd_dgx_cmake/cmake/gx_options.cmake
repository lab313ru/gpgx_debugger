# General compile options for DGX

option(GX_HOOK_CPU        "Enable CPU hook/debug core" ON) # Must be ON, it's a debugger after all
option(GX_ENABLE_CHD      "Enable CHD support via libchdr" ON)
option(GX_USE_TREMOR      "Use Tremor (libvorbis) sources in-tree" ON)

option(DEBUG_68K		  "Debug m68k otherwise z80" ON)