#pragma once
#include <cstdint>
#include <string_view>

namespace gx
{
	static std::uint32_t SOUND_FREQUENCY = 48000;
	static std::uint32_t SOUND_SAMPLES_SIZE = 2048;

	void init();
	bool load_rom(std::string_view in_rom);
	void shutdown();

	float target_frame_rate();

	std::uint8_t*& bitmap_data(); 
}
