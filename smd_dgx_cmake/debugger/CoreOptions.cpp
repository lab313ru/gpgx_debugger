// The option table and everything that does not touch the emulator core.
// The core side lives in CoreOptionsCore.cpp: the core headers and <windows.h>
// do not mix in one translation unit.

#include "CoreOptions.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace coreopt {

namespace {

std::vector<Choice> numbers(int from, int to, int step)
{
    std::vector<Choice> v;
    for (int n = from; n <= to; n += step) {
        const std::string s = std::to_string(n);
        v.push_back({ s, s, n });
    }
    return v;
}

std::vector<Choice> onOff()
{
    return { { "disabled", "Disabled", 0 }, { "enabled", "Enabled", 1 } };
}

std::vector<OptionDef> buildTable()
{
    using C = Category;
    using A = Apply;
    return {
        // ---- System ----
        { "genesis_plus_gx_region_detect", "System region", C::System, A::Live, &CoreOptions::region, {
            { "auto",      "Auto",      RegionAuto },
            { "ntsc-u",    "NTSC-U",    RegionUsa },
            { "pal",       "PAL",       RegionEurope },
            { "ntsc-j",    "NTSC-J",    RegionJapanNtsc },
            // Not a libretro value: the core has the region, libretro does not offer it.
            { "japan pal", "Japan PAL", RegionJapanPal } } },
        { "genesis_plus_gx_vdp_mode", "Force VDP mode", C::System, A::Live, &CoreOptions::vdpMode, {
            { "auto",  "Disabled",    VdpModeAuto },
            { "60hz",  "NTSC (60Hz)", VdpMode60Hz },
            { "50hz",  "PAL (50Hz)",  VdpMode50Hz } } },
        // Values are libretro's, the meaning is the Wii GUI's: see CoreOptions::lockups.
        { "genesis_plus_gx_force_dtack", "System lock-ups", C::System, A::Live, &CoreOptions::lockups, onOff() },
        { "genesis_plus_gx_addr_error", "68K address error", C::System, A::Live, &CoreOptions::addrError, onOff() },
        { "genesis_plus_gx_bios", "System boot ROM (TMSS)", C::System, A::Reset, &CoreOptions::bootRom, onOff() },
        { "genesis_plus_gx_lock_on", "Cartridge lock-on", C::System, A::Reset, &CoreOptions::lockOn, {
            { "disabled",            "Disabled",            LockOnOff },
            { "game genie",          "Game Genie",          LockOnGameGenie },
            { "action replay (pro)", "Action Replay (Pro)", LockOnActionReplay },
            { "sonic & knuckles",    "Sonic & Knuckles",    LockOnSonicKnuckles } } },
        { "genesis_plus_gx_system_hw", "System hardware", C::System, A::Reload, &CoreOptions::hardware, {
            { "auto",                 "Auto",               HardwareAuto },
            { "mega drive / genesis", "Mega Drive/Genesis", HardwareMegaDrive } } },

        // ---- Audio ----
        { "genesis_plus_gx_ym2612", "Mega Drive FM", C::Audio, A::Live, &CoreOptions::fmCore, {
            { "mame (ym2612)",          "MAME (YM2612)",          FmMameYm2612 },
            { "mame (asic ym3438)",     "MAME (ASIC YM3438)",     FmMameAsicYm3438 },
            { "mame (enhanced ym3438)", "MAME (Enhanced YM3438)", FmMameEnhancedYm3438 },
            { "nuked (ym2612)",         "Nuked (YM2612)",         FmNukedYm2612 },
            { "nuked (ym3438)",         "Nuked (YM3438)",         FmNukedYm3438 } } },
        { "genesis_plus_gx_sound_output", "Sound output", C::Audio, A::Live, &CoreOptions::mono, {
            { "stereo", "Stereo", 0 },
            { "mono",   "Mono",   1 } } },
        { "genesis_plus_gx_audio_filter", "Audio filter", C::Audio, A::Live, &CoreOptions::filter, {
            { "disabled", "Disabled", FilterOff },
            { "low-pass", "Low-Pass", FilterLowPass },
            { "EQ",       "EQ",       FilterEq } } },
        { "genesis_plus_gx_lowpass_range", "Low-pass filter %", C::Audio, A::Live, &CoreOptions::lowPassRange, numbers(5, 95, 5) },
        { "genesis_plus_gx_psg_preamp", "PSG preamp level", C::Audio, A::Live, &CoreOptions::psgPreamp, numbers(0, 200, 5) },
        { "genesis_plus_gx_fm_preamp", "FM preamp level", C::Audio, A::Live, &CoreOptions::fmPreamp, numbers(0, 200, 5) },
        { "genesis_plus_gx_audio_eq_low", "EQ low", C::Audio, A::Live, &CoreOptions::eqLow, numbers(0, 100, 5) },
        { "genesis_plus_gx_audio_eq_mid", "EQ mid", C::Audio, A::Live, &CoreOptions::eqMid, numbers(0, 100, 5) },
        { "genesis_plus_gx_audio_eq_high", "EQ high", C::Audio, A::Live, &CoreOptions::eqHigh, numbers(0, 100, 5) },

        // ---- Video ----
        { "genesis_plus_gx_overscan", "Borders", C::Video, A::Live, &CoreOptions::overscan, {
            { "disabled",   "Disabled",   OverscanOff },
            { "top/bottom", "Top/Bottom", OverscanTopBottom },
            { "left/right", "Left/Right", OverscanLeftRight },
            { "full",       "Full",       OverscanFull } } },
        { "genesis_plus_gx_blargg_ntsc_filter", "Blargg NTSC filter", C::Video, A::Live, &CoreOptions::ntscFilter, {
            { "disabled",   "Disabled",   NtscOff },
            { "monochrome", "Monochrome", NtscMonochrome },
            { "composite",  "Composite",  NtscComposite },
            { "svideo",     "S-Video",    NtscSVideo },
            { "rgb",        "RGB",        NtscRgb } } },
        { "genesis_plus_gx_lcd_filter", "LCD ghosting filter", C::Video, A::Live, &CoreOptions::lcdGhosting, onOff() },
        { "genesis_plus_gx_render", "Interlaced mode 2 output", C::Video, A::Live, &CoreOptions::doubleField, {
            { "single field", "Single Field", 0 },
            { "double field", "Double Field", 1 } } },
        { "genesis_plus_gx_aspect_ratio", "Aspect ratio", C::Video, A::Live, &CoreOptions::aspect, {
            { "auto",     "Auto",     AspectAuto },
            { "NTSC PAR", "NTSC PAR", AspectNtscPar },
            { "PAL PAR",  "PAL PAR",  AspectPalPar } } },
    };
}

std::filesystem::path exeDir()
{
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);        // truncated: long path
    }
    return std::filesystem::path(buf).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p.parent_path();
#else
    return {};
#endif
}

} // namespace

const std::vector<OptionDef>& options()
{
    static const std::vector<OptionDef> table = buildTable();
    return table;
}

const OptionDef* findOption(std::string_view key)
{
    for (const auto& d : options())
        if (key == d.key) return &d;
    return nullptr;
}

std::string valueOf(const CoreOptions& o, const OptionDef& def)
{
    const int v = o.*def.field;
    for (const auto& c : def.choices)
        if (c.stored == v) return c.value;
    return {};
}

bool setValue(CoreOptions& o, const OptionDef& def, std::string_view value)
{
    for (const auto& c : def.choices)
        if (value == c.value) { o.*def.field = c.stored; return true; }
    return false;
}

Apply needs(const CoreOptions& from, const CoreOptions& to)
{
    Apply worst = Apply::Live;
    for (const auto& d : options())
        if (from.*d.field != to.*d.field && d.apply > worst) worst = d.apply;
    return worst;
}

CoreOptions withClass(const CoreOptions& base, const CoreOptions& src, Apply cls)
{
    CoreOptions out = base;
    for (const auto& d : options())
        if (d.apply == cls) out.*d.field = src.*d.field;
    return out;
}

BootRom findBootRom()
{
    constexpr const char* kName = "bios_MD.bin";
    constexpr size_t kSize = 0x800;

    BootRom r;
    std::vector<std::filesystem::path> where;
    if (auto d = exeDir(); !d.empty()) where.push_back(d / kName);
    where.push_back(std::filesystem::path(kName));     // working directory

    std::error_code ec;
    std::filesystem::path found;
    for (const auto& p : where)
        if (std::filesystem::is_regular_file(p, ec)) { found = p; break; }
    if (found.empty()) {
        r.problem = std::string(kName) + " not found beside the executable or in the working "
                    "directory: TMSS runs without the boot ROM";
        return r;
    }
    const std::u8string u8 = found.u8string();
    r.path.assign(u8.begin(), u8.end());

    std::vector<uint8_t> img(kSize, 0xFF);
    std::ifstream in(found, std::ios::binary);     // a path, not a narrow string: any user dir works
    in.read(reinterpret_cast<char*>(img.data()), (std::streamsize)kSize);
    const size_t got = (size_t)in.gcount();
    // The check the core's own frontends make before trusting the file.
    if (got < 0x12A || std::memcmp(img.data() + 0x120, "GENESIS OS", 10) != 0) {
        r.problem = r.path + " is not a Mega Drive boot ROM: TMSS runs without it";
        return r;
    }
    r.image = std::move(img);
    return r;
}

} // namespace coreopt
