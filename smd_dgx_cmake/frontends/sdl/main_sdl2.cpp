#include <SDL2/SDL.h>
#undef main

#include "save_state.hpp"


extern "C"
{
#include "sms_ntsc.h"
#include "md_ntsc.h"
#include "shared.h"
}

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <SDL2/SDL_opengl.h>

#include <cstdint>
#include <type_traits>

template <typename T>
inline typename std::enable_if<sizeof(T) == 1, T>::type
byteswap(T val) {
	return val;
}

template <typename T>
inline typename std::enable_if<sizeof(T) == 2, T>::type
byteswap(T val) {
	auto u = static_cast<uint16_t>(val);
	u = (u >> 8) | (u << 8);
	return static_cast<T>(u);
}

template <typename T>
inline typename std::enable_if<sizeof(T) == 4, T>::type
byteswap(T val) {
	auto u = static_cast<uint32_t>(val);
	u = ((u & 0x000000FF) << 24) |
		((u & 0x0000FF00) << 8) |
		((u & 0x00FF0000) >> 8) |
		((u & 0xFF000000) >> 24);
	return static_cast<T>(u);
}

uint16_t read_word_be(uint8* data, size_t offset)
{
	return (data[offset] << 8) | data[offset + 1];
}

uint8_t read_byte_from_word_aligned(uint8* data, size_t offset)
{
	size_t word_offset = offset & ~1;
	uint16_t word = read_word_be(data, word_offset);

	if ((offset & 1))
		return word >> 8;
	else
		return word & 0x00FF;
}

#define SOUND_FREQUENCY 48000
#define SOUND_SAMPLES_SIZE  2048

#define VIDEO_WIDTH  320
#define VIDEO_HEIGHT 224

#define WINDOW_WIDTH 960
#define WINDOW_HEIGHT 720

#define GL_CHECK() \
    { \
        GLenum err; \
        while ((err = glGetError()) != GL_NO_ERROR) { \
            printf("[GL_ERROR] %s:%d: 0x%04X\n", __FILE__, __LINE__, err); \
        } \
    }

int joynum = 0;

int log_error = 0;
int debug_on = 0;
int turbo_mode = 0;
int use_sound = 1;
int fullscreen = 0; /* SDL_WINDOW_FULLSCREEN */

struct {
	SDL_Window* window;
	SDL_GLContext gl_context;
#ifndef DONT_RENDER
	GLuint frame_texture = 0;
	SDL_Surface* surf_bitmap;
	SDL_Rect srect;
	SDL_Rect drect;
#endif
	Uint32 frames_rendered;
} sdl_video;

/* sound */
struct audio_driver_t
{
	static bool is_ready()
	{
		constexpr auto start_memory = 0xFF0000;
		constexpr auto driver_is_ready_offset = 0xFFFFFE - start_memory;

		return read_byte_from_word_aligned(work_ram, driver_is_ready_offset);
	}

	static void play_debug_bgm()
	{
		constexpr uint16_t command = 0x1115;

		send_command(command);
	}


	static void send_command(uint16_t in_command)
	{
		commands.push_back(in_command);
	}

	static void flush_buffer()
	{
		if (!commands.empty())
		{
			constexpr uint8_t cmd_buffer_size = 0x20 - 1;
			constexpr auto start_memory = 0xFF0000;
			auto write_index = byteswap(read_word_be(work_ram, 0xFFFF00 - start_memory));
			auto&& cmd_buffer_ptr = reinterpret_cast<uint16_t*>(work_ram + (0xFFFF02 - start_memory));

			while (cmd_buffer_ptr[write_index]) // while commands looking for empty space
			{
				write_index++;
			}
			write_index &= cmd_buffer_size;

			for (auto&& command : commands)
			{
				command = command | 0x1000;
				cmd_buffer_ptr[write_index++] = command;
				write_index &= cmd_buffer_size;
			}
			commands.clear();
		}
	}

	static bool is_all_muted()
	{
		constexpr auto driver_state_offset = 0x1B;
		return work_ram[0x1B] & 0x1;
	}

	static uint16_t check_audio_state()
	{
		uint16_t out_audio_state = 0;

		if (is_all_muted())
			out_audio_state = 0x1FF;

		constexpr uint32_t check_channels_addresses[] = {
			0x23,
			0xED,
			0x1B7,
			0x415,
			0x49F,
			0x529,
			0x281,
			0x34B,
			0x5B3,
			0x629,
			0x6F3,
		};

		for (size_t i = 0; i < std::size(check_channels_addresses); ++i)
		{
			auto&& addr = check_channels_addresses[i];
			auto&& channel_state = work_ram[addr];
			if (channel_state & 0x10)
			{
				out_audio_state |= (1 << i);
			}
		}

		return out_audio_state;
	}

	static std::vector<uint16_t> commands;
};

struct {
	char* current_pos;
	char* buffer;
	int current_emulated_samples;
} sdl_sound;


static uint8 brm_format[0x40] =
{
  0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00,
  0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
};


static short soundframe[SOUND_SAMPLES_SIZE];

static void sdl_sound_callback(void* userdata, Uint8* stream, int len)
{
	if (sdl_sound.current_emulated_samples < len) {
		memset(stream, 0, len);
	}
	else {
		memcpy(stream, sdl_sound.buffer, len);
		/* loop to compensate desync */
		do {
			sdl_sound.current_emulated_samples -= len;
		} while (sdl_sound.current_emulated_samples > 2 * len);
		memcpy(sdl_sound.buffer,
			sdl_sound.current_pos - sdl_sound.current_emulated_samples,
			sdl_sound.current_emulated_samples);
		sdl_sound.current_pos = sdl_sound.buffer + sdl_sound.current_emulated_samples;
	}
}

static int sdl_sound_init()
{
	int n;
	SDL_AudioSpec as_desired;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Audio initialization failed", sdl_video.window);
		return 0;
	}

	as_desired.freq = SOUND_FREQUENCY;
	as_desired.format = AUDIO_S16SYS;
	as_desired.channels = 2;
	as_desired.samples = SOUND_SAMPLES_SIZE;
	as_desired.callback = sdl_sound_callback;

	if (SDL_OpenAudio(&as_desired, NULL) < 0) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Audio open failed", sdl_video.window);
		return 0;
	}

	sdl_sound.current_emulated_samples = 0;
	n = SOUND_SAMPLES_SIZE * 2 * sizeof(short) * 20;
	sdl_sound.buffer = (char*)malloc(n);
	if (!sdl_sound.buffer) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Can't allocate audio buffer", sdl_video.window);
		return 0;
	}
	memset(sdl_sound.buffer, 0, n);
	sdl_sound.current_pos = sdl_sound.buffer;
	return 1;
}

static void sdl_sound_update(int enabled)
{
	int size = audio_update(soundframe) * 2;

	if (enabled)
	{
		int i;
		short* out;

		SDL_LockAudio();
		out = (short*)sdl_sound.current_pos;
		for (i = 0; i < size; i++)
		{
			*out++ = soundframe[i];
		}
		sdl_sound.current_pos = (char*)out;
		sdl_sound.current_emulated_samples += size * sizeof(short);
		SDL_UnlockAudio();
	}
}

static void sdl_sound_close()
{
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	if (sdl_sound.buffer)
		free(sdl_sound.buffer);
}

extern "C"
{
	/* video */
	md_ntsc_t* md_ntsc;
	sms_ntsc_t* sms_ntsc;
}

static int sdl_video_init()
{
#if defined(USE_8BPP_RENDERING)
	const unsigned long surface_format = SDL_PIXELFORMAT_RGB332;
#elif defined(USE_15BPP_RENDERING)
	const unsigned long surface_format = SDL_PIXELFORMAT_RGB555;
#elif defined(USE_16BPP_RENDERING)
	const unsigned long surface_format = SDL_PIXELFORMAT_RGB565;
#elif defined(USE_32BPP_RENDERING)
	const unsigned long surface_format = SDL_PIXELFORMAT_RGB888;
#endif

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Video initialization failed", sdl_video.window);
		return 0;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

	sdl_video.window = SDL_CreateWindow("Genesis Plus GX", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		WINDOW_WIDTH, WINDOW_HEIGHT, fullscreen | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	sdl_video.gl_context = SDL_GL_CreateContext(sdl_video.window);
	SDL_GL_MakeCurrent(sdl_video.window, sdl_video.gl_context);
	SDL_GL_SetSwapInterval(1);

#ifndef DONT_RENDER
	glGenTextures(1, &sdl_video.frame_texture);
	glBindTexture(GL_TEXTURE_2D, sdl_video.frame_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); 
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, 720, 576, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, nullptr);

	sdl_video.surf_bitmap = SDL_CreateRGBSurfaceWithFormat(0, 720, 576, SDL_BITSPERPIXEL(surface_format), surface_format);
	sdl_video.frames_rendered = 0;
#endif
	SDL_ShowCursor(0);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;

	ImGui::StyleColorsDark();

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 0.0f;
	style.Colors[ImGuiCol_WindowBg].w = 1.0f;

	ImGui_ImplSDL2_InitForOpenGL(sdl_video.window, sdl_video.gl_context);
	ImGui_ImplOpenGL3_Init("#version 330");

	return 1;
}

#ifndef DONT_RENDER
static void draw_game()
{
	if (auto&& viewport = ImGui::GetMainViewport())
	{
		const auto src_w = (float)bitmap.viewport.w, src_h = (float)bitmap.viewport.h;
		const auto dst_w = viewport->Size.x, dst_h = viewport->Size.y;

		const auto scale = std::min(dst_w / src_w, dst_h / src_h);
		const auto draw_w = src_w * scale, draw_h = src_h * scale;
		const auto pad_x = (dst_w - draw_w) * 0.5f, pad_y = (dst_h - draw_h) * 0.5f;

		const auto pos0 = ImVec2(viewport->Pos.x + pad_x, viewport->Pos.y + pad_y);
		const auto pos1 = ImVec2(pos0.x + draw_w, pos0.y + draw_h);

		const auto u0 = (float)bitmap.viewport.x / bitmap.width;
		const auto v0 = (float)bitmap.viewport.y / bitmap.height;
		const auto u1 = (float)(bitmap.viewport.x + bitmap.viewport.w) / bitmap.width;
		const auto v1 = (float)(bitmap.viewport.y + bitmap.viewport.h) / bitmap.height;

		ImGui::GetBackgroundDrawList(viewport)->AddImage(
			(ImTextureID)(intptr_t)sdl_video.frame_texture,
			pos0, pos1,
			ImVec2(u0, v0), ImVec2(u1, v1)
		);
	}
}
#endif

void draw_input()
{
	ImGui::Begin("Input Pad");

	const uint16_t pad = input.pad[joynum];

	struct { uint16_t bit; const char* name; } buttons[] = {
		{ INPUT_UP,    "UP"    },
		{ INPUT_DOWN,  "DOWN"  },
		{ INPUT_LEFT,  "LEFT"  },
		{ INPUT_RIGHT, "RIGHT" },
		{ INPUT_A,     "A"     },
		{ INPUT_B,     "B"     },
		{ INPUT_C,     "C"     },
		{ INPUT_X,     "X"     },
		{ INPUT_Y,     "Y"     },
		{ INPUT_Z,     "Z"     },
		{ INPUT_START, "START" },
		{ INPUT_MODE,  "MODE"  },
	};

	ImGui::Text("D-Pad:");
	ImGui::SameLine();
	ImGui::TextColored((pad & INPUT_UP) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 1, 1, 1), "UP");
	ImGui::SameLine();
	ImGui::TextColored((pad & INPUT_DOWN) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 1, 1, 1), "DOWN");
	ImGui::SameLine();
	ImGui::TextColored((pad & INPUT_LEFT) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 1, 1, 1), "LEFT");
	ImGui::SameLine();
	ImGui::TextColored((pad & INPUT_RIGHT) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 1, 1, 1), "RIGHT");

	ImGui::Separator();

	ImGui::Text("Buttons:");
	for (int i = 4; i < IM_ARRAYSIZE(buttons); ++i)
	{
		if (i > 4) ImGui::SameLine();
		ImGui::TextColored((pad & buttons[i].bit) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 1, 1, 1), "%s", buttons[i].name);
	}

	ImGui::End();
}

void ShowInputPadWindow()
{
	ImGui::Begin("SEGA Input Pad");

	uint16_t pad = input.pad[joynum];

	auto Btn = [&](bool pressed, const char* label, ImU32 color = IM_COL32(255, 255, 255, 255)) {
		ImVec4 col = pressed ? ImVec4(0.1f, 0.9f, 0.1f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, col);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x, col.y, col.z, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x * 0.8f, col.y * 0.8f, col.z * 0.8f, 1.0f));
		ImGui::Button(label, ImVec2(32, 32));
		ImGui::PopStyleColor(3);
		};

	ImGui::Columns(3, nullptr, false);

	// -- LEFT: D-PAD
	{
		ImGui::SetColumnWidth(0, 110.0f);
		ImGui::Dummy(ImVec2(32, 4));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 38);
		Btn(pad & INPUT_UP, "↑");
		ImGui::Dummy(ImVec2(0, 2));

		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);
		Btn(pad & INPUT_LEFT, "←"); ImGui::SameLine(0, 4);
		Btn(pad & INPUT_DOWN, "↓"); ImGui::SameLine(0, 4);
		Btn(pad & INPUT_RIGHT, "→");

		ImGui::Dummy(ImVec2(32, 10));
	}
	ImGui::NextColumn();

	// -- MIDDLE: START + MODE
	{
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 24);
		Btn(pad & INPUT_START, "START");
		ImGui::Dummy(ImVec2(0, 10));
		Btn(pad & INPUT_MODE, "MODE");
		ImGui::Dummy(ImVec2(0, 40));
	}
	ImGui::NextColumn();

	// -- RIGHT: XYZ (верх) и ABC (низ)
	{
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
		Btn(pad & INPUT_X, "X"); ImGui::SameLine(0, 8);
		Btn(pad & INPUT_Y, "Y"); ImGui::SameLine(0, 8);
		Btn(pad & INPUT_Z, "Z");

		ImGui::Dummy(ImVec2(0, 12));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
		Btn(pad & INPUT_A, "A"); ImGui::SameLine(0, 8);
		Btn(pad & INPUT_B, "B"); ImGui::SameLine(0, 8);
		Btn(pad & INPUT_C, "C");
	}
	ImGui::Columns(1);

	ImGui::End();
}

static std::string read_string(size_t offset, size_t len)
{
	std::string out_string;
	for (size_t i = 0; i < len; i += 2)
	{
		uint8_t hi = ext.md_cart.rom[offset + i];
		uint8_t lo = ext.md_cart.rom[offset + i + 1];
		out_string += static_cast<char>(lo);
		out_string += static_cast<char>(hi);
	}

	return out_string;
}

template<typename T>
T wrap(T value, T min, T max)
{
	if (value < min)
	{
		return max;
	}
	if (value > max)
	{
		return min;
	}
	return value;
}

static void draw_audio_player()
{
	ImGui::Begin("Audio");

	ImGui::SeparatorText("BGM");
	{
		constexpr int8_t bgm_num = 34;
		static int8_t bgm_index = 0;
		if (ImGui::ArrowButton("previous_bgm", ImGuiDir_Left))
		{
			bgm_index = wrap(bgm_index - 1, 0, bgm_num - 1);
		}
		ImGui::SameLine();
		constexpr auto string_len = 0x14;
		const auto bgm_name_offset = 0x0003EDA0 + static_cast<uint32_t>(bgm_index) * string_len;
		auto&& bgm_name = read_string(bgm_name_offset, string_len);
		ImGui::Text("%s", bgm_name.data()); ImGui::SameLine();
		if (ImGui::ArrowButton("next_bgm", ImGuiDir_Right))
		{
			bgm_index = wrap(bgm_index + 1, 0, bgm_num - 1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Play BGM"))
		{
			constexpr auto bgm_index_offset = 0x0003ED7E;
			const auto actual_track_index = read_byte_from_word_aligned(ext.md_cart.rom, bgm_index_offset + bgm_index);

			audio_driver_t::send_command(0x100 + actual_track_index);
		}
	}

	ImGui::SeparatorText("SFX");
	{
		constexpr int8_t sfx_num = 96;
		static int8_t sfx_index = 0;
	}

	ImGui::SeparatorText("PCM");
	{
		constexpr int8_t pcm_num = 96;
		static int8_t pcm_index = 0;
		if (ImGui::ArrowButton("previous_pcm", ImGuiDir_Left))
		{
			pcm_index = wrap(pcm_index - 1, 0, pcm_num - 1);
		}
		ImGui::SameLine();
		ImGui::Text("%i", pcm_index); ImGui::SameLine();
		if (ImGui::ArrowButton("next_pcm", ImGuiDir_Right))
		{
			pcm_index = wrap(pcm_index + 1, 0, pcm_num - 1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Play PCM"))
		{
			constexpr auto pcm_index_offset = 0x0003F048;
			const auto actual_track_index = read_byte_from_word_aligned(ext.md_cart.rom, pcm_index_offset + pcm_index);

			audio_driver_t::send_command(0x200 + actual_track_index);
		}
	}

	ImGui::Separator();

	if (ImGui::Button("Stop All"))
	{
		audio_driver_t::send_command(0x0);
	}

	ImGui::SeparatorText("Audio State");
	ImGui::Text("Is All Muted: %x", audio_driver_t::is_all_muted());
	ImGui::Text("Channels State: %x", audio_driver_t::check_audio_state());
	ImGui::Text("Is Playing: %i", (audio_driver_t::check_audio_state() & 0x7FF) != 0x7FF);

	ImGui::End();
}

static void sdl_video_update()
{
	if (system_hw == SYSTEM_MCD)
	{
		system_frame_scd(false);
	}
	else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
	{
		system_frame_gen(false);
	}
	else
	{
		system_frame_sms(false);
	}

#ifndef DONT_RENDER
	/* viewport size changed */
	if (bitmap.viewport.changed & 1)
	{
		bitmap.viewport.changed &= ~1;
	
		/* source bitmap */
		sdl_video.srect.w = bitmap.viewport.w + 2 * bitmap.viewport.x;
		sdl_video.srect.h = bitmap.viewport.h + 2 * bitmap.viewport.y;
		sdl_video.srect.x = 0;
		sdl_video.srect.y = 0;
		if (sdl_video.srect.w > sdl_video.surf_bitmap->w)
		{
			sdl_video.srect.x = (sdl_video.srect.w - sdl_video.surf_bitmap->w) / 2;
			sdl_video.srect.w = sdl_video.surf_bitmap->w;
		}
		if (sdl_video.srect.h > sdl_video.surf_bitmap->h)
		{
			sdl_video.srect.y = (sdl_video.srect.h - sdl_video.surf_bitmap->h) / 2;
			sdl_video.srect.h = sdl_video.surf_bitmap->h;
		}
	
		/* destination bitmap */
		sdl_video.drect.w = sdl_video.srect.w;
		sdl_video.drect.h = sdl_video.srect.h;
		sdl_video.drect.x = (sdl_video.surf_bitmap->w - sdl_video.drect.w) / 2;
		sdl_video.drect.y = (sdl_video.surf_bitmap->h - sdl_video.drect.h) / 2;
	}
#endif
	
	int window_w, window_h;
	SDL_GetWindowSize(sdl_video.window, &window_w, &window_h);
	glViewport(0, 0, window_w, window_h);
	
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

#ifndef DONT_RENDER
	glBindTexture(GL_TEXTURE_2D, sdl_video.frame_texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bitmap.width, bitmap.height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, bitmap.data);
#endif
	
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

#ifndef DONT_RENDER
	draw_game();
#endif
	draw_input();
	draw_audio_player();
	
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	
	ImGuiIO& io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) 
	{
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	
		SDL_GL_MakeCurrent(sdl_video.window, sdl_video.gl_context);
	}
	
	SDL_GL_SwapWindow(sdl_video.window);
	
	++sdl_video.frames_rendered;
}

static void sdl_video_close()
{
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
#ifndef DONT_RENDER
	SDL_FreeSurface(sdl_video.surf_bitmap);
#endif
	SDL_DestroyWindow(sdl_video.window);
}

struct sdl_sync_t {
	Uint64 last_counter;
	Uint64 interval_ticks;
	int ticks;
} sdl_sync;

static int sdl_sync_init()
{
	if (SDL_InitSubSystem(SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Timer initialization failed", sdl_video.window);
		return 0;
	}

	sdl_sync.last_counter = SDL_GetPerformanceCounter();
	double interval_ms = vdp_pal ? 60.0 : 50.0;
	sdl_sync.interval_ticks = (Uint64)(SDL_GetPerformanceFrequency() * (interval_ms / 1000.0));
	sdl_sync.ticks = 0;

	return 1;
}

void sdl_sync_update_fps_event()
{
	Uint64 now = SDL_GetPerformanceCounter();
	Uint64 delta = now - sdl_sync.last_counter;
	
	if (delta >= sdl_sync.interval_ticks)
	{
		int ticks_passed = (int)(delta / sdl_sync.interval_ticks);
		sdl_sync.last_counter += sdl_sync.interval_ticks * ticks_passed;
		sdl_sync.ticks += ticks_passed;
	
		const int target_ticks = vdp_pal ? 50 : 20;
	
		if (sdl_sync.ticks >= target_ticks)
		{
			SDL_Event event;
			SDL_UserEvent userevent{};
	
			userevent.type = SDL_USEREVENT;
			userevent.code = vdp_pal ? (sdl_video.frames_rendered / 3) : sdl_video.frames_rendered;
			userevent.data1 = nullptr;
			userevent.data2 = nullptr;
	
			event.type = SDL_USEREVENT;
			event.user = userevent;
	
			SDL_PushEvent(&event);
	
			sdl_sync.ticks = 0;
			sdl_video.frames_rendered = 0;
		}
	}
}

static void sdl_sync_close()
{
}

static const uint16 vc_table[4][2] =
{
	/* NTSC, PAL */
	{0xDA , 0xF2},  /* Mode 4 (192 lines) */
	{0xEA , 0x102}, /* Mode 5 (224 lines) */
	{0xDA , 0xF2},  /* Mode 4 (192 lines) */
	{0x106, 0x10A}  /* Mode 5 (240 lines) */
};

static int sdl_control_update(SDL_Keycode keystate)
{
	switch (keystate)
	{
	case SDLK_TAB:
	{
		system_reset();
		break;
	}
	case SDLK_SPACE:
	{
		if (audio_driver_t::is_ready())
		{
			audio_driver_t::play_debug_bgm();
		}
		break;
	}

	case SDLK_F1:
	{
		if (SDL_ShowCursor(-1)) SDL_ShowCursor(0);
		else SDL_ShowCursor(1);
		break;
	}

	case SDLK_F2:
	{
		fullscreen = (fullscreen ? 0 : SDL_WINDOW_FULLSCREEN);
		SDL_SetWindowFullscreen(sdl_video.window, fullscreen);
		bitmap.viewport.changed = 1;
		break;
	}

	case SDLK_F3:
	{
		if (config.bios == 0) config.bios = 3;
		else if (config.bios == 3) config.bios = 1;
		break;
	}

	case SDLK_F4:
	{
		if (!turbo_mode) use_sound ^= 1;
		break;
	}

	case SDLK_F5:
	{
		log_error ^= 1;
		break;
	}

	case SDLK_F6:
	{
		if (!use_sound)
		{
			turbo_mode ^= 1;
			sdl_sync.ticks = 0;
		}
		break;
	}

	case SDLK_F7:
	{
		genesis::SaveState::handle_result(genesis::SaveState::load("game"));
		break;
	}

	case SDLK_F8:
	{
		genesis::SaveState::handle_result(genesis::SaveState::save("game"));
		break;
	}

	case SDLK_F9:
	{
		config.region_detect = (config.region_detect + 1) % 5;
		get_region(0);

		/* framerate has changed, reinitialize audio timings */
		audio_init(snd.sample_rate, 0);

		/* system with region BIOS should be reinitialized */
		if ((system_hw == SYSTEM_MCD) || ((system_hw & SYSTEM_SMS) && (config.bios & 1)))
		{
			system_init();
			system_reset();
		}
		else
		{
			/* reinitialize I/O region register */
			if (system_hw == SYSTEM_MD)
			{
				io_reg[0x00] = 0x20 | region_code | (config.bios & 1);
			}
			else
			{
				io_reg[0x00] = 0x80 | (region_code >> 1);
			}

			/* reinitialize VDP */
			if (vdp_pal)
			{
				status |= 1;
				lines_per_frame = 313;
			}
			else
			{
				status &= ~1;
				lines_per_frame = 262;
			}

			/* reinitialize VC max value */
			switch (bitmap.viewport.h)
			{
			case 192:
				vc_max = vc_table[0][vdp_pal];
				break;
			case 224:
				vc_max = vc_table[1][vdp_pal];
				break;
			case 240:
				vc_max = vc_table[3][vdp_pal];
				break;
			}
		}
		break;
	}

	case SDLK_F10:
	{
		gen_reset(0);
		break;
	}

	case SDLK_F11:
	{
		config.overscan = (config.overscan + 1) & 3;
		if ((system_hw == SYSTEM_GG) && !config.gg_extra)
		{
			bitmap.viewport.x = (config.overscan & 2) ? 14 : -48;
		}
		else
		{
			bitmap.viewport.x = (config.overscan & 2) * 7;
		}
		bitmap.viewport.changed = 3;
		break;
	}

	case SDLK_F12:
	{
		joynum = (joynum + 1) % MAX_DEVICES;
		while (input.dev[joynum] == NO_DEVICE)
		{
			joynum = (joynum + 1) % MAX_DEVICES;
		}
		break;
	}

	case SDLK_ESCAPE:
	{
		return 0;
	}

	default:
		break;
	}

	return 1;
}

int sdl_input_update(void)
{
	const uint8* keystate = SDL_GetKeyboardState(NULL);
	int window_w, window_h;
	SDL_GetWindowSize(sdl_video.window, &window_w, &window_h);

	/* reset input */
	input.pad[joynum] = 0;

	switch (input.dev[joynum])
	{
	case DEVICE_LIGHTGUN:
	{
		/* get mouse coordinates (absolute values) */
		int x, y;
		int state = SDL_GetMouseState(&x, &y);

		/* X axis */
		input.analog[joynum][0] = x - (window_w - bitmap.viewport.w) / 2;

		/* Y axis */
		input.analog[joynum][1] = y - (window_h - bitmap.viewport.h) / 2;

		/* TRIGGER, B, C (Menacer only), START (Menacer & Justifier only) */
		if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_A;
		if (state & SDL_BUTTON_RMASK) input.pad[joynum] |= INPUT_B;
		if (state & SDL_BUTTON_MMASK) input.pad[joynum] |= INPUT_C;
		if (keystate[SDL_SCANCODE_F])  input.pad[joynum] |= INPUT_START;
		break;
	}

	case DEVICE_PADDLE:
	{
		/* get mouse (absolute values) */
		int x;
		int state = SDL_GetMouseState(&x, NULL);

		/* Range is [0;256], 128 being middle position */
		input.analog[joynum][0] = x * 256 / window_w;

		/* Button I -> 0 0 0 0 0 0 0 I*/
		if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_B;

		break;
	}

	case DEVICE_SPORTSPAD:
	{
		/* get mouse (relative values) */
		int x, y;
		int state = SDL_GetRelativeMouseState(&x, &y);

		/* Range is [0;256] */
		input.analog[joynum][0] = (unsigned char)(-x & 0xFF);
		input.analog[joynum][1] = (unsigned char)(-y & 0xFF);

		/* Buttons I & II -> 0 0 0 0 0 0 II I*/
		if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_B;
		if (state & SDL_BUTTON_RMASK) input.pad[joynum] |= INPUT_C;

		break;
	}

	case DEVICE_MOUSE:
	{
		/* get mouse (relative values) */
		int x, y;
		int state = SDL_GetRelativeMouseState(&x, &y);

		/* Sega Mouse range is [-256;+256] */
		input.analog[joynum][0] = x * 2;
		input.analog[joynum][1] = y * 2;

		/* Vertical movement is upsidedown */
		if (!config.invert_mouse)
			input.analog[joynum][1] = 0 - input.analog[joynum][1];

		/* Start,Left,Right,Middle buttons -> 0 0 0 0 START MIDDLE RIGHT LEFT */
		if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_B;
		if (state & SDL_BUTTON_RMASK) input.pad[joynum] |= INPUT_C;
		if (state & SDL_BUTTON_MMASK) input.pad[joynum] |= INPUT_A;
		if (keystate[SDL_SCANCODE_F])  input.pad[joynum] |= INPUT_START;

		break;
	}

	case DEVICE_XE_1AP:
	{
		/* A,B,C,D,Select,START,E1,E2 buttons -> E1(?) E2(?) START SELECT(?) A B C D */
		if (keystate[SDL_SCANCODE_A])  input.pad[joynum] |= INPUT_START;
		if (keystate[SDL_SCANCODE_S])  input.pad[joynum] |= INPUT_A;
		if (keystate[SDL_SCANCODE_D])  input.pad[joynum] |= INPUT_C;
		if (keystate[SDL_SCANCODE_F])  input.pad[joynum] |= INPUT_Y;
		if (keystate[SDL_SCANCODE_Z])  input.pad[joynum] |= INPUT_B;
		if (keystate[SDL_SCANCODE_X])  input.pad[joynum] |= INPUT_X;
		if (keystate[SDL_SCANCODE_C])  input.pad[joynum] |= INPUT_MODE;
		if (keystate[SDL_SCANCODE_V])  input.pad[joynum] |= INPUT_Z;

		/* Left Analog Stick (bidirectional) */
		if (keystate[SDL_SCANCODE_UP])     input.analog[joynum][1] -= 2;
		else if (keystate[SDL_SCANCODE_DOWN])   input.analog[joynum][1] += 2;
		else input.analog[joynum][1] = 128;
		if (keystate[SDL_SCANCODE_LEFT])   input.analog[joynum][0] -= 2;
		else if (keystate[SDL_SCANCODE_RIGHT])  input.analog[joynum][0] += 2;
		else input.analog[joynum][0] = 128;

		/* Right Analog Stick (unidirectional) */
		if (keystate[SDL_SCANCODE_KP_8])    input.analog[joynum + 1][0] -= 2;
		else if (keystate[SDL_SCANCODE_KP_2])   input.analog[joynum + 1][0] += 2;
		else if (keystate[SDL_SCANCODE_KP_4])   input.analog[joynum + 1][0] -= 2;
		else if (keystate[SDL_SCANCODE_KP_6])  input.analog[joynum + 1][0] += 2;
		else input.analog[joynum + 1][0] = 128;

		/* Limiters */
		if (input.analog[joynum][0] > 0xFF) input.analog[joynum][0] = 0xFF;
		else if (input.analog[joynum][0] < 0) input.analog[joynum][0] = 0;
		if (input.analog[joynum][1] > 0xFF) input.analog[joynum][1] = 0xFF;
		else if (input.analog[joynum][1] < 0) input.analog[joynum][1] = 0;
		if (input.analog[joynum + 1][0] > 0xFF) input.analog[joynum + 1][0] = 0xFF;
		else if (input.analog[joynum + 1][0] < 0) input.analog[joynum + 1][0] = 0;
		if (input.analog[joynum + 1][1] > 0xFF) input.analog[joynum + 1][1] = 0xFF;
		else if (input.analog[joynum + 1][1] < 0) input.analog[joynum + 1][1] = 0;

		break;
	}

	case DEVICE_PICO:
	{
		/* get mouse (absolute values) */
		int x, y;
		int state = SDL_GetMouseState(&x, &y);

		/* Calculate X Y axis values */
		input.analog[0][0] = 0x3c + (x * (0x17c - 0x03c + 1)) / window_w;
		input.analog[0][1] = 0x1fc + (y * (0x2f7 - 0x1fc + 1)) / window_h;

		/* Map mouse buttons to player #1 inputs */
		if (state & SDL_BUTTON_MMASK) pico_current = (pico_current + 1) & 7;
		if (state & SDL_BUTTON_RMASK) input.pad[0] |= INPUT_PICO_RED;
		if (state & SDL_BUTTON_LMASK) input.pad[0] |= INPUT_PICO_PEN;

		break;
	}

	case DEVICE_TEREBI:
	{
		/* get mouse (absolute values) */
		int x, y;
		int state = SDL_GetMouseState(&x, &y);

		/* Calculate X Y axis values */
		input.analog[0][0] = (x * 250) / window_w;
		input.analog[0][1] = (y * 250) / window_h;

		/* Map mouse buttons to player #1 inputs */
		if (state & SDL_BUTTON_RMASK) input.pad[0] |= INPUT_B;

		break;
	}

	case DEVICE_GRAPHIC_BOARD:
	{
		/* get mouse (absolute values) */
		int x, y;
		int state = SDL_GetMouseState(&x, &y);

		/* Calculate X Y axis values */
		input.analog[0][0] = (x * 255) / window_w;
		input.analog[0][1] = (y * 255) / window_h;

		/* Map mouse buttons to player #1 inputs */
		if (state & SDL_BUTTON_LMASK) input.pad[0] |= INPUT_GRAPHIC_PEN;
		if (state & SDL_BUTTON_RMASK) input.pad[0] |= INPUT_GRAPHIC_MENU;
		if (state & SDL_BUTTON_MMASK) input.pad[0] |= INPUT_GRAPHIC_DO;

		break;
	}

	case DEVICE_ACTIVATOR:
	{
		if (keystate[SDL_SCANCODE_G])  input.pad[joynum] |= INPUT_ACTIVATOR_7L;
		if (keystate[SDL_SCANCODE_H])  input.pad[joynum] |= INPUT_ACTIVATOR_7U;
		if (keystate[SDL_SCANCODE_J])  input.pad[joynum] |= INPUT_ACTIVATOR_8L;
		if (keystate[SDL_SCANCODE_K])  input.pad[joynum] |= INPUT_ACTIVATOR_8U;
	}

	default:
	{
		if (keystate[SDL_SCANCODE_A])  input.pad[joynum] |= INPUT_A;
		if (keystate[SDL_SCANCODE_S])  input.pad[joynum] |= INPUT_B;
		if (keystate[SDL_SCANCODE_D])  input.pad[joynum] |= INPUT_C;
		if (keystate[SDL_SCANCODE_F])  input.pad[joynum] |= INPUT_START;
		if (keystate[SDL_SCANCODE_Z])  input.pad[joynum] |= INPUT_X;
		if (keystate[SDL_SCANCODE_X])  input.pad[joynum] |= INPUT_Y;
		if (keystate[SDL_SCANCODE_C])  input.pad[joynum] |= INPUT_Z;
		if (keystate[SDL_SCANCODE_V])  input.pad[joynum] |= INPUT_MODE;

		if (keystate[SDL_SCANCODE_UP]) input.pad[joynum] |= INPUT_UP;
		else
			if (keystate[SDL_SCANCODE_DOWN]) input.pad[joynum] |= INPUT_DOWN;
		if (keystate[SDL_SCANCODE_LEFT]) input.pad[joynum] |= INPUT_LEFT;
		else
			if (keystate[SDL_SCANCODE_RIGHT]) input.pad[joynum] |= INPUT_RIGHT;

		break;
	}
	}

	return 1;
}

static void pc_changed(unsigned int pc)
{
}

std::vector<uint16_t> audio_driver_t::commands;

int main(int argc, char** argv)
{
	FILE* fp;
	int running = 1;

	/* Print help if no game specified */
	if (argc < 2)
	{
		char caption[256];
		sprintf(caption, "Genesis Plus GX\\SDL\nusage: %s gamename\n", argv[0]);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Information", caption, sdl_video.window);
		return 1;
	}

	/* set default config */
	error_init();
	set_config_defaults();

	/* mark all BIOS as unloaded */
	system_bios = 0;

	/* Genesis BOOT ROM support (2KB max) */
	memset(boot_rom, 0xFF, 0x800);
	fp = fopen(MD_BIOS, "rb");
	if (fp != NULL)
	{
		int i;

		/* read BOOT ROM */
		fread(boot_rom, 1, 0x800, fp);
		fclose(fp);

		/* check BOOT ROM */
		if (!memcmp((char*)(boot_rom + 0x120), "GENESIS OS", 10))
		{
			/* mark Genesis BIOS as loaded */
			system_bios = SYSTEM_MD;
		}

		/* Byteswap ROM */
		for (i = 0; i < 0x800; i += 2)
		{
			uint8 temp = boot_rom[i];
			boot_rom[i] = boot_rom[i + 1];
			boot_rom[i + 1] = temp;
		}
	}

	/* initialize SDL */
	if (SDL_Init(0) < 0)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL initialization failed", sdl_video.window);
		return 1;
	}
	sdl_video_init();
	genesis::set_save_state_message_box([](const char* title, const char* msg) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, sdl_video.window);
	});
	if (use_sound) sdl_sound_init();
	sdl_sync_init();

#ifndef DONT_RENDER
	/* initialize Genesis virtual system */
	SDL_LockSurface(sdl_video.surf_bitmap);
#endif
	memset(&bitmap, 0, sizeof(t_bitmap));
	bitmap.width = 720;
	bitmap.height = 576;
#if defined(USE_8BPP_RENDERING)
	bitmap.pitch = (bitmap.width * 1);
#elif defined(USE_15BPP_RENDERING)
	bitmap.pitch = (bitmap.width * 2);
#elif defined(USE_16BPP_RENDERING)
	bitmap.pitch = (bitmap.width * 2);
#elif defined(USE_32BPP_RENDERING)
	bitmap.pitch = (bitmap.width * 4);
#endif
#ifndef DONT_RENDER
	bitmap.data = reinterpret_cast<uint8*>(sdl_video.surf_bitmap->pixels);
	SDL_UnlockSurface(sdl_video.surf_bitmap);
#endif
	bitmap.viewport.changed = 3;

	/* Load game file */
	if (!load_rom(argv[1]))
	{
		char caption[256];
		sprintf(caption, "Error loading file `%s'.", argv[1]);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", caption, sdl_video.window);
		return 1;
	}

	/* initialize system hardware */
	audio_init(SOUND_FREQUENCY, 0);
	system_init();

	// cpu_set_fc_callback(pc_changed);

	/* Mega CD specific */
	if (system_hw == SYSTEM_MCD)
	{
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

	if (use_sound) SDL_PauseAudio(0);

	const double target_fps = vdp_pal ? 50.0 : 60.0;
	const double frame_time = 1000.0 / target_fps;

	Uint64 perf_freq = SDL_GetPerformanceFrequency();
	double error_correction = 0.0;

	/* emulation loop */
	while (running)
	{
		Uint64 start_counter = SDL_GetPerformanceCounter();

		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			ImGui_ImplSDL2_ProcessEvent(&event);

			switch (event.type)
			{
			case SDL_USEREVENT:
			{
				char caption[100];
				sprintf(caption, "Genesis Plus GX - %d fps - %s", event.user.code, (rominfo.international[0] != 0x20) ? rominfo.international : rominfo.domestic);
				SDL_SetWindowTitle(sdl_video.window, caption);
				continue;
			}

			case SDL_QUIT:
			{
				running = 0;
				continue;
			}

			case SDL_KEYDOWN:
			{
				running = sdl_control_update(event.key.keysym.sym);
			}
			}
		}

		audio_driver_t::flush_buffer();

		sdl_video_update();
		sdl_sound_update(use_sound);

		sdl_sync_update_fps_event();

		Uint64 end_counter = SDL_GetPerformanceCounter();
		double elapsed = (double)(end_counter - start_counter) * 1000.0 / perf_freq;

		double wait_time = frame_time - elapsed + error_correction;

		if (wait_time > 0.0)
		{
			Uint32 wait_ms = (Uint32)wait_time;
			SDL_Delay(wait_ms);

			Uint64 after_sleep = SDL_GetPerformanceCounter();
			double actual_waited = (double)(after_sleep - end_counter) * 1000.0 / perf_freq;
			error_correction = wait_time - actual_waited;
		}
		else
		{
			error_correction = 0.0;
		}
	}

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

	sdl_video_close();
	sdl_sound_close();
	sdl_sync_close();
	SDL_Quit();

	return 0;
}
