#include "gx/gx.hpp"

#include <cstring>

extern "C" {
#include <shared.h>
#include <genesis.h>
#include <loadrom.h>
#include <scd.h>
#include <sram.h>
#include <vdp_ctrl.h>
#include <error.h>
}

#include "config.h"
#include "system.h"

namespace 
{
	uint8 brm_format[0x40] =
	{
	  0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40,
	  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	  0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00,
	  0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
	};
}

void gx::init()
{
	error_init();
	set_config_defaults();

	/* mark all BIOS as unloaded */
	system_bios = 0;

	std::memset(&bitmap, 0, sizeof(t_bitmap));
	bitmap.width = 720;
	bitmap.height = 576;
#ifdef USE_8BPP_RENDERING
	bitmap.pitch = (bitmap.width * 1);
#elif defined(USE_15BPP_RENDERING)
	bitmap.pitch = (bitmap.width * 2);
#elif defined(USE_16BPP_RENDERING)
	bitmap.pitch = (bitmap.width * 2);
#elif defined(USE_32BPP_RENDERING/**/)
	bitmap.pitch = (bitmap.width * 4);
#endif
	bitmap.viewport.changed = 3;
}

bool gx::load_rom(std::string_view in_rom)
{
	if (::load_rom(const_cast<char*>(in_rom.data())))
	{
		/* initialize system hardware */
		audio_init(gx::SOUND_FREQUENCY, 0);
		system_init();

		/* Mega CD specific */
		if (system_hw == SYSTEM_MCD)
		{
			FILE* fp;
			/* load internal backup RAM */
			fp = fopen("./scd.brm", "rb");
			if (fp != NULL)
			{
				fread(scd.bram, 0x2000, 1, fp);
				fclose(fp);
			}

			/* check if internal backup RAM is formatted */
			if (memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
			{
				/* clear internal backup RAM */
				memset(scd.bram, 0x00, 0x200);

				/* Internal Backup RAM size fields */
				brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = 0x00;
				brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (sizeof(scd.bram) / 64) - 3;

				/* format internal backup RAM */
				memcpy(scd.bram + 0x2000 - 0x40, brm_format, 0x40);
			}

			/* load cartridge backup RAM */
			if (scd.cartridge.id)
			{
				fp = fopen("./cart.brm", "rb");
				if (fp != NULL)
				{
					fread(scd.cartridge.area, scd.cartridge.mask + 1, 1, fp);
					fclose(fp);
				}

				/* check if cartridge backup RAM is formatted */
				if (memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
				{
					/* clear cartridge backup RAM */
					memset(scd.cartridge.area, 0x00, scd.cartridge.mask + 1);

					/* Cartridge Backup RAM size fields */
					brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = (((scd.cartridge.mask + 1) / 64) - 3) >> 8;
					brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (((scd.cartridge.mask + 1) / 64) - 3) & 0xff;

					/* format cartridge backup RAM */
					memcpy(scd.cartridge.area + scd.cartridge.mask + 1 - sizeof(brm_format), brm_format, sizeof(brm_format));
				}
			}
		}

		if (sram.on)
		{
			FILE* fp;
			/* load SRAM */
			fp = fopen("./game.srm", "rb");
			if (fp != NULL)
			{
				fread(sram.sram, 0x10000, 1, fp);
				fclose(fp);
			}
		}

		/* reset system hardware */
		system_reset();

		return true;
	}
	return false;
}

void gx::shutdown()
{
	FILE* fp;
	if (system_hw == SYSTEM_MCD)
	{
		/* save internal backup RAM (if formatted) */
		if (!memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
		{
			fp = fopen("./scd.brm", "wb");
			if (fp != NULL)
			{
				fwrite(scd.bram, 0x2000, 1, fp);
				fclose(fp);
			}
		}

		/* save cartridge backup RAM (if formatted) */
		if (scd.cartridge.id)
		{
			if (!memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
			{
				fp = fopen("./cart.brm", "wb");
				if (fp != NULL)
				{
					fwrite(scd.cartridge.area, scd.cartridge.mask + 1, 1, fp);
					fclose(fp);
				}
			}
		}
	}

	if (sram.on)
	{
		/* save SRAM */
		fp = fopen("./game.srm", "wb");
		if (fp != NULL)
		{
			fwrite(sram.sram, 0x10000, 1, fp);
			fclose(fp);
		}
	}

	audio_shutdown();
	error_shutdown();
}

float gx::target_frame_rate()
{
	return vdp_pal ? 50.0f : 60.0f;
}

std::uint8_t*& gx::bitmap_data()
{
	return bitmap.data;
}
