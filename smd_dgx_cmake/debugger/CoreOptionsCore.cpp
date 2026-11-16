// The core side of CoreOptions: every function here writes emulator globals
// and runs on the emulation thread. Each follows what libretro.c does for the
// same option, so the core sees the sequence of writes it was written for.

#include "CoreOptions.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <gx/gx.hpp>     // SOUND_FREQUENCY, the rate gx::load_rom opens audio at

extern "C" {
#include <shared.h>      // config, system/VDP/IO globals — must be first
#include <md_ntsc.h>
#include <sms_ntsc.h>

// Defined by the frontend (frontends/core/config.c) and read by the renderer
// whenever config.ntsc is set.
extern md_ntsc_t*  md_ntsc;
extern sms_ntsc_t* sms_ntsc;
}

namespace coreopt {

namespace {

uint32 lowPassFactor(int percent) { return (uint32)(percent * 65536 / 100); }   // 0.16 fixed point
uint8  lcdFactor(int on)          { return on ? (uint8)(0.80 * 256) : 0; }       // 0.8 fixed point

// Nothing allocates the filter's tables but this: the renderer dereferences
// them on every line drawn with config.ntsc set. Allocated on first use and
// kept for the life of the process.
bool ensureNtscTables()
{
    if (!md_ntsc)  md_ntsc  = static_cast<md_ntsc_t*>(std::calloc(1, sizeof(md_ntsc_t)));
    if (!sms_ntsc) sms_ntsc = static_cast<sms_ntsc_t*>(std::calloc(1, sizeof(sms_ntsc_t)));
    return md_ntsc && sms_ntsc;
}

void setNtsc(int filter)
{
    if (filter == NtscOff || !ensureNtscTables()) { config.ntsc = 0; return; }
    switch (filter) {
    case NtscMonochrome:
        sms_ntsc_init(sms_ntsc, &sms_ntsc_monochrome);
        md_ntsc_init(md_ntsc, &md_ntsc_monochrome);
        break;
    case NtscComposite:
        sms_ntsc_init(sms_ntsc, &sms_ntsc_composite);
        md_ntsc_init(md_ntsc, &md_ntsc_composite);
        break;
    case NtscSVideo:
        sms_ntsc_init(sms_ntsc, &sms_ntsc_svideo);
        md_ntsc_init(md_ntsc, &md_ntsc_svideo);
        break;
    default:
        sms_ntsc_init(sms_ntsc, &sms_ntsc_rgb);
        md_ntsc_init(md_ntsc, &md_ntsc_rgb);
        break;
    }
    config.ntsc = 1;
}

// config.ym2612 picks the MAME core's flavour, config.ym3438 picks Nuked over
// MAME (1 = YM2612, 2 = YM3438). The Nuked chip type is the core's own static,
// which nothing on the load path sets.
void setFmCore(int core)
{
    switch (core) {
    case FmMameYm2612:         config.ym3438 = 0; config.ym2612 = YM2612_DISCRETE;   break;
    case FmMameAsicYm3438:     config.ym3438 = 0; config.ym2612 = YM2612_INTEGRATED; break;
    case FmMameEnhancedYm3438: config.ym3438 = 0; config.ym2612 = YM2612_ENHANCED;   break;
    case FmNukedYm2612:
        config.ym3438 = 1; config.ym2612 = YM2612_ENHANCED;
        OPN2_SetChipType(ym3438_mode_ym2612);
        break;
    case FmNukedYm3438:
        config.ym3438 = 2; config.ym2612 = YM2612_ENHANCED;
        OPN2_SetChipType(ym3438_mode_readmode);
        break;
    }
}

// The renderer recomputes the active area from these at the next frame.
void refreshViewport()
{
    bitmap.viewport.changed = 11;
    if ((system_hw == SYSTEM_GG) && !config.gg_extra)
        bitmap.viewport.x = (config.overscan & 2) ? 14 : -48;
    else
        bitmap.viewport.x = (config.overscan & 2) * 7;
}

// Region and VDP mode on a running console. Returns true when the console has
// to be re-initialised instead: its BIOS is region specific.
bool applyRegion(const CoreOptions& to)
{
    config.region_detect = (uint8)to.region;
    config.vdp_mode      = (uint8)to.vdpMode;
    if (!system_hw) return false;

    get_region(nullptr);     // forced region and timing over the ROM's own
    if ((system_hw == SYSTEM_MCD) || ((system_hw & SYSTEM_SMS) && config.bios))
        return true;

    static const uint16 vcTable[4][2] = {
        // NTSC, PAL
        { 0xDA,  0xF2  },    // Mode 4 (192 lines)
        { 0xEA,  0x102 },    // Mode 5 (224 lines)
        { 0xDA,  0xF2  },    // Mode 4 (192 lines)
        { 0x106, 0x10A },    // Mode 5 (240 lines)
    };

    // The frame rate may have changed, and with it the samples per frame.
    audio_set_rate((int)gx::SOUND_FREQUENCY, 0);

    // The version register the game reads its region from.
    if (system_hw == SYSTEM_MD)
        io_reg[0x00] = 0x20 | region_code | (config.bios & 1);
    else
        io_reg[0x00] = 0x80 | (region_code >> 1);

    lines_per_frame = vdp_pal ? 313 : 262;
    if (system_hw & SYSTEM_MD)
        status = (status & ~1) | vdp_pal;      // PAL flag in the VDP status

    switch (bitmap.viewport.h) {
    case 192: vc_max = vcTable[0][vdp_pal]; break;
    case 224: vc_max = vcTable[1][vdp_pal]; break;
    case 240: vc_max = vcTable[3][vdp_pal]; break;
    }
    return false;
}

} // namespace

void applyAtLoad(const CoreOptions& o)
{
    config.region_detect = (uint8)o.region;
    config.vdp_mode      = (uint8)o.vdpMode;
    config.force_dtack   = o.lockups ? 0 : 1;
    config.addr_error    = (uint8)o.addrError;      // gen_init copies it into the 68000
    applyResetFields(o);
    config.system        = o.hardware == HardwareMegaDrive ? SYSTEM_MD : 0;

    // sound_init/sound_reset and audio_reset read these on the way up.
    setFmCore(o.fmCore);
    config.mono       = (uint8)o.mono;
    config.filter     = (uint8)o.filter;
    config.lp_range   = lowPassFactor(o.lowPassRange);
    config.psg_preamp = (int16)o.psgPreamp;
    config.fm_preamp  = (int16)o.fmPreamp;
    config.lg = (int16)o.eqLow;
    config.mg = (int16)o.eqMid;
    config.hg = (int16)o.eqHigh;

    // vdp_reset derives the viewport from these.
    config.overscan = (uint8)o.overscan;
    setNtsc(o.ntscFilter);
    config.lcd    = lcdFactor(o.lcdGhosting);
    config.render = (uint8)o.doubleField;
}

bool applyLive(const CoreOptions& from, const CoreOptions& to)
{
    bool reinit = false;
    bool viewport = false;

    if (from.region != to.region || from.vdpMode != to.vdpMode) {
        reinit = applyRegion(to);
        viewport = !reinit;
    }
    if (from.lockups != to.lockups)
        config.force_dtack = to.lockups ? 0 : 1;
    if (from.addrError != to.addrError)
        m68k.aerr_enabled = config.addr_error = (uint8)to.addrError;

    // Levels and filters before the FM core: a core switch resets the PSG,
    // which reads its preamp as it goes.
    if (from.mono != to.mono)
        config.mono = (uint8)to.mono;
    if (from.filter != to.filter)
        config.filter = (uint8)to.filter;
    if (from.lowPassRange != to.lowPassRange)
        config.lp_range = lowPassFactor(to.lowPassRange);
    if (from.psgPreamp != to.psgPreamp) {
        config.psg_preamp = (int16)to.psgPreamp;
        if (system_hw)
            psg_config(0, config.psg_preamp, ((system_hw & SYSTEM_PBC) == SYSTEM_MD) ? 0xff : io_reg[6]);
    }
    if (from.fmPreamp != to.fmPreamp)
        config.fm_preamp = (int16)to.fmPreamp;
    if (from.eqLow != to.eqLow || from.eqMid != to.eqMid || from.eqHigh != to.eqHigh) {
        config.lg = (int16)to.eqLow;
        config.mg = (int16)to.eqMid;
        config.hg = (int16)to.eqHigh;
        if (system_hw) audio_set_equalizer();
    }
    if (from.fmCore != to.fmCore) {
        const bool wasNuked = config.ym3438 != 0;
        setFmCore(to.fmCore);
        YM2612Config(config.ym2612);
        // Between MAME and Nuked the chip itself is replaced, and starts silent.
        if (system_hw && wasNuked != (config.ym3438 != 0)) {
            sound_init();
            sound_reset();
        }
    }

    if (from.overscan != to.overscan) {
        config.overscan = (uint8)to.overscan;
        viewport = true;
    }
    if (from.ntscFilter != to.ntscFilter) {
        const uint8 was = config.ntsc;
        setNtsc(to.ntscFilter);
        viewport |= was != config.ntsc;
    }
    if (from.lcdGhosting != to.lcdGhosting)
        config.lcd = lcdFactor(to.lcdGhosting);
    if (from.doubleField != to.doubleField) {
        config.render = (uint8)to.doubleField;
        viewport = true;
    }

    if (viewport && system_hw) refreshViewport();
    return reinit;
}

void applyResetFields(const CoreOptions& o)
{
    // 3, not 1: bit 1 makes a Master System BIOS hand over to the cartridge.
    config.bios    = o.bootRom ? 3 : 0;
    config.lock_on = (uint8)o.lockOn;
}

void installBootRom(const BootRom* rom)
{
    if (!rom || rom->image.size() != sizeof boot_rom) {
        system_bios &= ~SYSTEM_MD;
        return;
    }
    std::memcpy(boot_rom, rom->image.data(), sizeof boot_rom);
#ifdef LSB_FIRST
    for (size_t i = 0; i < sizeof boot_rom; i += 2) {      // the core's 68000 word order
        const uint8 t = boot_rom[i];
        boot_rom[i] = boot_rom[i + 1];
        boot_rom[i + 1] = t;
    }
#endif
    system_bios |= SYSTEM_MD;
}

void reinitMachine()
{
    // system_init rebuilds the cartridge, and with it clears SRAM the game has
    // written; a power cycle on the real console keeps it.
    std::vector<uint8> keep(sram.sram, sram.sram + sizeof sram.sram);
    audio_init((int)gx::SOUND_FREQUENCY, 0);
    system_init();
    system_reset();
    std::memcpy(sram.sram, keep.data(), keep.size());
    refreshViewport();
}

void softReset()
{
    gen_reset(0);
}

Picture picture(const CoreOptions& o)
{
    // remap_line draws every line from x = 0, border included, and puts the
    // top border's lines first.
    const int w = bitmap.viewport.w + 2 * bitmap.viewport.x;
    const int h = bitmap.viewport.h + 2 * bitmap.viewport.y;
    Picture p;
    if (w <= 0 || h <= 0) return p;

    // calculate_display_aspect_ratio, from the picture before the filter and
    // the second field change its size. The sample rates cut a line into
    // square pixels of a 480- or 576-line frame, and one console line is a
    // field line, two frame lines tall.
    if (o.aspect == AspectAuto && (system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS)
        && config.overscan == 0 && config.gg_extra == 0) {
        p.aspect = (6.0 / 5.0) * w / h;
    } else {
        constexpr double kNtscRate = 135000000.0 / 11.0;
        constexpr double kPalRate  = 14750000.0;
        const double dotRate = system_clock / (bitmap.viewport.w == 320 ? 8.0 : 10.0);
        const double rate = o.aspect == AspectNtscPar ? kNtscRate
                          : o.aspect == AspectPalPar  ? kPalRate
                          : vdp_pal ? kPalRate : kNtscRate;
        p.aspect = (rate / dotRate) * (double(w) / (h * 2.0));
    }

    // The same choice of filter remap_line makes for the line.
    p.width  = !config.ntsc ? w : (reg[12] & 1) ? MD_NTSC_OUT_WIDTH(w) : SMS_NTSC_OUT_WIDTH(w);
    p.height = (config.render && interlaced) ? h * 2 : h;
    p.width  = std::min(p.width, (int)bitmap.width);
    p.height = std::min(p.height, (int)bitmap.height);
    return p;
}

} // namespace coreopt
