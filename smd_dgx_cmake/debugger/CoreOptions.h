#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Emulator core options for the standalone app.
//
// The subset of the libretro core's options (libretro/libretro_core_options.h)
// this build supports, under the libretro keys and value strings, so a stored
// setting reads the same way a RetroArch one does.
//
// Defaults are this program's behaviour before the options existed — what
// set_config_defaults() gives the core — not libretro's defaults where the two
// differ: the audio filter is low-pass here and off there.
//
// This header has no core dependency, so a UI can build its menus from it.
// The core-side functions at the bottom touch emulator globals and must run on
// the emulation thread.
//
// Only a host that hands EmuHost a CoreOptions applies any of this. The IDA
// plugin never does, and runs on the core defaults.

namespace coreopt {

// What it takes for a changed option to reach the game.
enum class Apply {
    Live,     // applied between two frames; the game keeps running
    Reset,    // read when the console powers on: takes a hard reset
    Reload,   // read while the ROM is loaded: takes loading it again
};

enum class Category { System, Audio, Video };

// Each enum's value is the option's index in its value list. Where a core
// field exists with the same numbering, the comment names it.
enum Region      { RegionAuto, RegionUsa, RegionEurope, RegionJapanNtsc, RegionJapanPal };  // config.region_detect
enum VdpMode     { VdpModeAuto, VdpMode60Hz, VdpMode50Hz };                                 // config.vdp_mode
enum LockOn      { LockOnOff, LockOnGameGenie, LockOnActionReplay, LockOnSonicKnuckles };   // config.lock_on (TYPE_*)
enum Hardware    { HardwareAuto, HardwareMegaDrive };
enum FmCore      { FmMameYm2612, FmMameAsicYm3438, FmMameEnhancedYm3438, FmNukedYm2612, FmNukedYm3438 };
enum AudioFilter { FilterOff, FilterLowPass, FilterEq };                                     // config.filter
enum Overscan    { OverscanOff, OverscanTopBottom, OverscanLeftRight, OverscanFull };        // config.overscan
enum NtscFilter  { NtscOff, NtscMonochrome, NtscComposite, NtscSVideo, NtscRgb };
enum AspectRatio { AspectAuto, AspectNtscPar, AspectPalPar };

// Every field is an int so one table (options()) can read and write any of
// them through a member pointer. Switches are 0/1.
struct CoreOptions {
    // System
    int region    = RegionAuto;
    int vdpMode   = VdpModeAuto;
    // In the Wii GUI's sense: 1 = the console locks up on an access real
    // hardware never acknowledges, i.e. config.force_dtack == 0. libretro's
    // "System Lock-Ups: enabled" sets force_dtack = 1, which turns them OFF;
    // the key is libretro's, the meaning is not.
    int lockups   = 1;
    int addrError = 1;
    int bootRom   = 0;           // TMSS, with bios_MD.bin when it can be found
    int lockOn    = LockOnOff;
    int hardware  = HardwareAuto;

    // Audio
    int fmCore       = FmMameYm2612;
    int mono         = 0;
    int filter       = FilterLowPass;
    int lowPassRange = 60;       // percent
    int psgPreamp    = 150;      // percent
    int fmPreamp     = 100;      // percent
    int eqLow        = 100;      // percent
    int eqMid        = 100;
    int eqHigh       = 100;

    // Video
    int overscan    = OverscanOff;
    int ntscFilter  = NtscOff;
    int lcdGhosting = 0;
    int doubleField = 0;         // interlaced mode 2: 0 single field, 1 double field
    int aspect      = AspectAuto;   // the front end's business: no core field

    bool operator==(const CoreOptions&) const = default;
};

struct Choice {
    std::string value;           // libretro value string, what gets stored
    std::string label;           // for a menu
    int         stored;          // the field value it stands for
};

struct OptionDef {
    const char*         key;     // libretro key
    const char*         label;
    Category            category;
    Apply               apply;
    int CoreOptions::*  field;
    std::vector<Choice> choices;
    // The stored values a menu offers, for a long numeric range; empty offers
    // every choice. Any listed value is still accepted from the settings file.
    std::vector<int>    presets = {};
};

// All options, in menu order within each category.
const std::vector<OptionDef>& options();
const OptionDef* findOption(std::string_view key);

// The value string for o's setting of def; empty if the field holds a value
// the option does not list.
std::string valueOf(const CoreOptions& o, const OptionDef& def);
// Sets def from a value string. Returns false — and leaves o alone — for a
// value the option does not list, so a stale or hand-edited setting falls back
// to the default instead of reaching the core.
bool setValue(CoreOptions& o, const OptionDef& def, std::string_view value);

// The strongest Apply class among the options that differ between the two;
// Live when nothing differs.
Apply needs(const CoreOptions& from, const CoreOptions& to);

// `base` with every option of class `cls` taken from `src`.
CoreOptions withClass(const CoreOptions& base, const CoreOptions& src, Apply cls);

// --- TMSS boot ROM --------------------------------------------------------

// Where bios_MD.bin is looked for: beside the executable, then the working
// directory — the place a user puts it and the place the SDL frontend reads.
struct BootRom {
    std::string          path;      // the file found, or empty
    std::vector<uint8_t> image;     // 2 KB as stored on disk, if valid
    std::string          problem;   // why there is no usable image, or empty
    std::string          brief;     // the same in a few words, for a menu
    bool ok() const { return problem.empty(); }
};
// File access only; safe on any thread.
BootRom findBootRom();

// The core reads a lock-on cartridge's ROM from the working directory
// (ggenie.bin, areplay.bin, sk.bin with sk2chip.bin) and silently runs without
// it when the file is not there. Both strings are empty when o.lockOn is off
// or has its files. File access only.
struct LockOnRom {
    std::string problem;   // why the lock-on cartridge has nothing to load
    std::string brief;     // the same in a few words, for a menu
    bool ok() const { return problem.empty(); }
};
LockOnRom checkLockOn(const CoreOptions& o);

// --- core side: emulation thread only ------------------------------------

// Writes every option into the core's config. Call after gx::init(), which
// resets config to defaults, and before ::load_rom(), which reads region,
// hardware, lock-on and TMSS while it builds the machine.
void applyAtLoad(const CoreOptions& o);

// Brings a running core from `from` to `to` for each LIVE option that
// differs, as libretro's check_variables() does. Call between frames.
// Returns true when the console has to be re-initialised regardless: a region
// change on a system whose BIOS is region specific. reinitMachine() does that.
bool applyLive(const CoreOptions& from, const CoreOptions& to);

// Writes the RESET-class options into config, for reinitMachine() to read.
void applyResetFields(const CoreOptions& o);

// Maps a boot ROM image in (or, with null, out). With TMSS on and no image the
// core still emulates TMSS itself: the VDP stays locked until the game writes
// "SEGA" to $A14000, with no logo screen before it.
void installBootRom(const BootRom* rom);

// libretro's "reinit": power-cycles the console, keeping the cartridge SRAM.
// This is the hard reset, and the one that picks up RESET-class options.
void reinitMachine();

// The console's reset button.
void softReset();

// The picture the core has drawn, as libretro hands it to its front end
// (update_viewport): it starts at the top left of bitmap.data and takes in the
// borders when they are on, the NTSC filter's wider lines and both fields of a
// double-field interlaced frame. The active area alone is bitmap.viewport.
struct Picture {
    int    width  = 0;
    int    height = 0;
    double aspect = 0;       // width / height on a TV, per o.aspect
};
Picture picture(const CoreOptions& o);

} // namespace coreopt
