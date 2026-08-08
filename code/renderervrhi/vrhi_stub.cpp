#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

// The vanilla clang-cl compatibility flags define __inline__ for a few C
// dependencies. SDL's Windows headers intentionally use the compiler builtin
// spelling, so isolate that macro while including SDL.
#ifdef __inline__
#pragma push_macro("__inline__")
#undef __inline__
#define VRHI_RESTORE_INLINE_MACRO
#endif
// Clang-cl exposes _m_prefetch as a builtin; SDL's compatibility shim is
// intended for older Clang versions and conflicts with that builtin.
#ifndef __PRFCHWINTRIN_H
#define __PRFCHWINTRIN_H
#define VRHI_RESTORE_PREFETCH_GUARD
#endif
#include <SDL.h>
#include <SDL_syswm.h>
#ifdef VRHI_RESTORE_PREFETCH_GUARD
#undef __PRFCHWINTRIN_H
#undef VRHI_RESTORE_PREFETCH_GUARD
#endif
#ifdef VRHI_RESTORE_INLINE_MACRO
#pragma pop_macro("__inline__")
#undef VRHI_RESTORE_INLINE_MACRO
#endif

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "q_shared.h"
#include "renderercommon/tr_public.h"

// vrhi.h is the public header for the copied prebuilt VRHI library.  The
// implementation deliberately uses only its device, swapchain, state,
// clear/readback/present entry points for now.
#include <vrhi.h>

namespace {

static refimport_t g_ri = {};
static SDL_Window *g_window = nullptr;
static HWND g_windowHandle = nullptr;
static bool g_sdlVideoActive = false;
static bool g_sdlVideoOwned = false;
static bool g_inputInitialized = false;
static bool g_deviceInitialized = false;
static bool g_uiInitialized = false;
static vhShader g_uiVertexShader = VRHI_INVALID_HANDLE;
static vhShader g_uiPixelShader = VRHI_INVALID_HANDLE;
static vhProgram g_uiProgram;
static vhState g_uiState;
static const vhStateId g_uiStateId = 2;
static glm::vec4 g_uiColor(1.0f, 1.0f, 1.0f, 1.0f);
static int g_frameViewportWidth = 0;
static int g_frameViewportHeight = 0;
static bool g_screenshotCommandRegistered = false;
static bool g_captureRequest = false;
static std::string g_captureName;
static bool g_frameBackbufferReady = false;
static int g_windowWidth = 1280;
static int g_windowHeight = 720;
static vhState g_frameState;
static vhTexture g_frameBackbuffer = VRHI_INVALID_HANDLE;
static const vhStateId g_frameStateId = 1;

// Keep the generated qpath within MAX_QPATH while allowing the command to
// accept only a basename.  The prefix and suffix are fixed and never come
// from the command line.
static const size_t VRHI_MAX_SCREENSHOT_NAME = MAX_QPATH - 16;

static void VRHI_FormatMessage(char *buffer, size_t bufferSize,
	const char *format, va_list args) {
	if (bufferSize == 0) {
		return;
	}
	vsnprintf(buffer, bufferSize, format, args);
	buffer[bufferSize - 1] = '\0';
}

static void VRHI_Printf(int printLevel, const char *format, ...) {
	char message[2048];
	va_list args;
	va_start(args, format);
	VRHI_FormatMessage(message, sizeof(message), format, args);
	va_end(args);

	if (g_ri.Printf != nullptr) {
		g_ri.Printf(printLevel, "%s", message);
	} else {
		fputs(message, stderr);
	}
}

static bool VRHI_Fatal(const char *format, ...) {
	char message[2048];
	va_list args;
	va_start(args, format);
	VRHI_FormatMessage(message, sizeof(message), format, args);
	va_end(args);

	if (g_ri.Error != nullptr) {
		g_ri.Error(ERR_FATAL, "%s", message);
	} else {
		fputs(message, stderr);
	}
	return false;
}

static void VRHI_Diagnostic(refimport_t *rimp, const char *message) {
	if (rimp != nullptr && rimp->Printf != nullptr) {
		rimp->Printf(PRINT_ALL, "%s", message);
		return;
	}
	fputs(message, stderr);
}

static void VRHI_CopyString(char *destination, size_t destinationSize,
	const std::string &source, const char *fallback) {
	const char *text = source.empty() ? fallback : source.c_str();
	if (destinationSize == 0) {
		return;
	}
	std::snprintf(destination, destinationSize, "%s", text);
	destination[destinationSize - 1] = '\0';
}

static int VRHI_CvarInteger(const char *name, const char *defaultValue) {
	if (g_ri.Cvar_Get != nullptr) {
		cvar_t *cvar = g_ri.Cvar_Get(name, defaultValue,
			CVAR_ARCHIVE | CVAR_LATCH);
		if (cvar != nullptr) {
			return cvar->integer;
		}
	}
	return std::atoi(defaultValue);
}

static void VRHI_GetRequestedResolution(int *width, int *height) {
	static const int modeWidths[] = {
		320, 400, 512, 640, 800, 960, 1024, 1152, 1280, 1600, 2048, 856
	};
	static const int modeHeights[] = {
		240, 300, 384, 480, 600, 720, 768, 864, 1024, 1200, 1536, 480
	};
	const int mode = VRHI_CvarInteger("r_mode", "-2");

	*width = 1280;
	*height = 720;
	if (mode == -1) {
		*width = VRHI_CvarInteger("r_customwidth", "1600");
		*height = VRHI_CvarInteger("r_customheight", "1024");
	} else if (mode >= 0 && mode < (int)(sizeof(modeWidths) / sizeof(modeWidths[0]))) {
		*width = modeWidths[mode];
		*height = modeHeights[mode];
	} else if (mode == -2) {
		SDL_DisplayMode displayMode;
		int display = 0;
		if (g_window != nullptr) {
			display = SDL_GetWindowDisplayIndex(g_window);
			if (display < 0) {
				display = 0;
			}
		}
		if (SDL_GetDesktopDisplayMode(display, &displayMode) == 0 &&
			displayMode.w > 0 && displayMode.h > 0) {
			*width = displayMode.w;
			*height = displayMode.h;
		}
	}

	if (*width <= 0 || *height <= 0) {
		*width = 1280;
		*height = 720;
	}
}

static bool VRHI_EnsureSDLVideo(void) {
	if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
		if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
			return VRHI_Fatal("renderer_vrhi: SDL video initialization failed: %s\n",
				SDL_GetError());
		}
		g_sdlVideoOwned = true;
	}
	g_sdlVideoActive = true;
	return true;
}

static bool VRHI_CreateWindow(int width, int height) {
	if (g_window != nullptr) {
		return true;
	}

	g_window = SDL_CreateWindow("ioquake3 VRHI",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	if (g_window == nullptr) {
		return VRHI_Fatal("renderer_vrhi: SDL Vulkan window creation failed: %s\n",
			SDL_GetError());
	}
	g_windowWidth = width;
	g_windowHeight = height;
	return true;
}

static bool VRHI_GetWindowHandle(void) {
	SDL_SysWMinfo windowInfo;
	std::memset(&windowInfo, 0, sizeof(windowInfo));
	SDL_VERSION(&windowInfo.version);
	if (SDL_GetWindowWMInfo(g_window, &windowInfo) != SDL_TRUE ||
		windowInfo.subsystem != SDL_SYSWM_WINDOWS ||
		windowInfo.info.win.window == nullptr) {
		return VRHI_Fatal("renderer_vrhi: SDL_GetWindowWMInfo failed: %s\n",
			SDL_GetError());
	}
	g_windowHandle = windowInfo.info.win.window;
	return true;
}

static bool VRHI_UpdateWindowSize(int requestedWidth, int requestedHeight,
	bool resizeNativeWindow) {
	int width = 0;
	int height = 0;
	const Uint32 windowFlags = SDL_GetWindowFlags(g_window);
	const bool minimized = (windowFlags & SDL_WINDOW_MINIMIZED) != 0;
	SDL_GetWindowSize(g_window, &width, &height);

	// A latched video mode is authoritative at registration time. Do not try to
	// resize a minimized window; vhFrame will report the temporary bad state.
	if (resizeNativeWindow && !minimized && width > 0 && height > 0 &&
		(width != requestedWidth || height != requestedHeight)) {
		SDL_SetWindowSize(g_window, requestedWidth, requestedHeight);
		SDL_GetWindowSize(g_window, &width, &height);
	}

	if (width > 0 && height > 0) {
		g_windowWidth = width;
		g_windowHeight = height;
		return true;
	}

	// SDL may report zero dimensions while a window is minimized. Retain the
	// last positive size for glconfig and skip vhResize in that case, but keep
	// registration alive so a minimized window does not become an init failure.
	return true;
}

static bool VRHI_RefreshSwapchain(void) {
	if (!g_deviceInitialized || g_window == nullptr) {
		return true;
	}

	const Uint32 windowFlags = SDL_GetWindowFlags(g_window);
	if ((windowFlags & SDL_WINDOW_MINIMIZED) != 0 ||
		g_windowWidth <= 0 || g_windowHeight <= 0) {
		return true;
	}

	const glm::uvec2 deviceSize = vhGetWindowSize();
	if (deviceSize.x == (uint32_t)g_windowWidth &&
		deviceSize.y == (uint32_t)g_windowHeight) {
		return true;
	}

	vhResize(g_windowWidth, g_windowHeight);
	g_vhInit.resolution = glm::ivec2(g_windowWidth, g_windowHeight);
	return true;
}

static bool VRHI_IsSafeScreenshotName(const char *name) {
	if (name == nullptr || name[0] == '\0') {
		return false;
	}

	size_t length = 0;
	for (const unsigned char *cursor =
		reinterpret_cast<const unsigned char *>(name);
		*cursor != '\0'; ++cursor) {
		if (!((*cursor >= 'a' && *cursor <= 'z') ||
			(*cursor >= 'A' && *cursor <= 'Z') ||
			(*cursor >= '0' && *cursor <= '9') ||
			*cursor == '_' || *cursor == '-')) {
			return false;
		}
		++length;
		if (length > VRHI_MAX_SCREENSHOT_NAME) {
			return false;
		}
	}
	return true;
}

static const char *VRHI_TextureFormatName(nvrhi::Format format) {
	const char *name = vhGetFormat(format).name;
	return name != nullptr ? name : "UNKNOWN";
}

static bool VRHI_IsScreenshotFormat(nvrhi::Format format, bool *inputBGRA) {
	if (inputBGRA != nullptr) {
		*inputBGRA = false;
	}

	switch (format) {
	case nvrhi::Format::RGBA8_UNORM:
	case nvrhi::Format::SRGBA8_UNORM:
		return true;
	case nvrhi::Format::BGRA8_UNORM:
	case nvrhi::Format::SBGRA8_UNORM:
	case nvrhi::Format::BGRX8_UNORM:
	case nvrhi::Format::SBGRX8_UNORM:
		if (inputBGRA != nullptr) {
			*inputBGRA = true;
		}
		return true;
	default:
		return false;
	}
}

static void VRHI_Screenshot_f(void) {
	const int argc = g_ri.Cmd_Argc != nullptr ? g_ri.Cmd_Argc() : 0;
	const char *name = "shot";
	if (argc > 2) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot: expected an optional safe name "
			"(alphanumeric, underscore, or hyphen)\n");
		return;
	}
	if (argc == 2) {
		name = g_ri.Cmd_Argv != nullptr ? g_ri.Cmd_Argv(1) : nullptr;
		if (!VRHI_IsSafeScreenshotName(name)) {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: screenshot: invalid name '%s'; expected "
				"alphanumeric, underscore, or hyphen\n",
				name != nullptr ? name : "(null)");
			return;
		}
	}

	// The command never accepts a qpath.  It stores only a validated basename;
	// EndFrame supplies the fixed screenshots/ prefix and .tga suffix.
	g_captureName = name;
	g_captureRequest = true;
	VRHI_Printf(PRINT_DEVELOPER,
		"renderer_vrhi: screenshot '%s' queued for the next cleared backbuffer\n",
		g_captureName.c_str());
}

static void VRHI_RegisterScreenshotCommand(void) {
	if (!g_screenshotCommandRegistered && g_ri.Cmd_AddCommand != nullptr) {
		g_ri.Cmd_AddCommand("screenshot", VRHI_Screenshot_f);
		g_screenshotCommandRegistered = true;
	}
}

static void VRHI_RemoveScreenshotCommand(void) {
	if (g_screenshotCommandRegistered && g_ri.Cmd_RemoveCommand != nullptr) {
		g_ri.Cmd_RemoveCommand("screenshot");
	}
	g_screenshotCommandRegistered = false;
}

static bool VRHI_WriteScreenshot(const std::string &name,
	const std::vector<byte> &tga, int width, int height) {
	if (!VRHI_IsSafeScreenshotName(name.c_str())) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': invalid internal name; write refused\n",
			name.c_str());
		return false;
	}
	if (tga.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': write failed for %dx%d TGA; "
			"file is too large (%zu bytes)\n",
			name.c_str(), width, height, tga.size());
		return false;
	}

	const std::string path = "screenshots/" + name + ".tga";
	if (g_ri.FS_WriteFile == nullptr) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': write failed for %s; "
			"FS_WriteFile is unavailable\n", name.c_str(), path.c_str());
		return false;
	}

	g_ri.FS_WriteFile(path.c_str(), tga.data(), static_cast<int>(tga.size()));
	if (g_ri.FS_FileExists == nullptr || !g_ri.FS_FileExists(path.c_str())) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': write failed for %s; "
			"filesystem did not confirm the output\n", name.c_str(), path.c_str());
		return false;
	}

	VRHI_Printf(PRINT_ALL, "renderer_vrhi: wrote %s (%dx%d)\n",
		path.c_str(), width, height);
	return true;
}

static bool VRHI_CaptureBackbuffer(void) {
	const std::string name = g_captureName;
	const vhTexture backbuffer = g_frameBackbuffer;
	if (backbuffer == VRHI_INVALID_HANDLE) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s' pending: invalid backbuffer handle "
			"0x%08x\n", name.c_str(), backbuffer);
		return false;
	}

	std::vector<vhTextureMipInfo> mipInfo;
	const vhTexInfo info = vhGetTextureInfo(backbuffer, &mipInfo);
	if (info.dimensions.x <= 0 || info.dimensions.y <= 0 ||
		info.dimensions.z != 1 || info.arrayLayers != 1 ||
		info.target != nvrhi::TextureDimension::Texture2D ||
		mipInfo.empty()) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': invalid texture metadata for "
			"backbuffer handle 0x%08x (target=%d dimensions=%dx%dx%d "
			"layers=%d mips=%zu)\n", name.c_str(), backbuffer,
			static_cast<int>(info.target), info.dimensions.x, info.dimensions.y,
			info.dimensions.z, info.arrayLayers, mipInfo.size());
		return false;
	}

	bool inputBGRA = false;
	if (!VRHI_IsScreenshotFormat(info.format, &inputBGRA)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': unsupported backbuffer format %s "
			"(%d) for handle 0x%08x; expected a 4-byte RGBA/BGRA UNORM or "
			"sRGB texture\n", name.c_str(), VRHI_TextureFormatName(info.format),
			static_cast<int>(info.format), backbuffer);
		return false;
	}
	const vhFormatInfo formatInfo = vhGetFormat(info.format);
	if (formatInfo.elementSize != 4 || formatInfo.compressionBlockWidth > 1 ||
		formatInfo.compressionBlockHeight > 1) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': unsupported backbuffer format %s "
			"(%d) for handle 0x%08x; expected an uncompressed 4-byte format\n",
			name.c_str(), VRHI_TextureFormatName(info.format),
			static_cast<int>(info.format), backbuffer);
		return false;
	}

	const vhTextureMipInfo &baseMip = mipInfo[0];
	const uint64_t width = static_cast<uint64_t>(info.dimensions.x);
	const uint64_t height = static_cast<uint64_t>(info.dimensions.y);
	const uint64_t rowBytes = width * 4;
	const uint64_t outputPixels = width * height;
	if (width > 65535 || height > 65535 ||
		rowBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
		outputPixels >
			(static_cast<uint64_t>(std::numeric_limits<size_t>::max()) - 18) / 3 ||
		baseMip.pitch <= 0 ||
		static_cast<uint64_t>(baseMip.pitch) < rowBytes ||
		baseMip.slice_size <= 0 ||
		static_cast<uint64_t>(baseMip.slice_size) >
			static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
		static_cast<uint64_t>(baseMip.slice_size) <
			static_cast<uint64_t>(baseMip.pitch) * height) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': invalid readback layout for "
			"backbuffer handle 0x%08x (dimensions=%ux%u pitch=%d slice=%lld)\n",
			name.c_str(), backbuffer, static_cast<unsigned>(width),
			static_cast<unsigned>(height), baseMip.pitch,
			static_cast<long long>(baseMip.slice_size));
		return false;
	}

	const size_t expectedReadbackBytes = static_cast<size_t>(baseMip.slice_size);
	const size_t outputBytes = 18 + static_cast<size_t>(outputPixels) * 3;
	vhMem readback;
	const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
	vhReadTextureSlow(backbuffer, 0, 0, &readback);
	vhFinish();
	const int32_t errorsAfter = g_vhErrorCounter.load(std::memory_order_relaxed);
	if (errorsAfter != errorsBefore || readback.size() != expectedReadbackBytes) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s': readback failed for backbuffer "
			"handle 0x%08x; got %zu bytes, expected %zu (VRHI errors %+d)\n",
			name.c_str(), backbuffer, readback.size(), expectedReadbackBytes,
			static_cast<int>(errorsAfter - errorsBefore));
		return false;
	}

	std::vector<byte> tga(outputBytes, 0);
	tga[2] = 2; // uncompressed true-color image
	tga[12] = static_cast<byte>(width & 0xff);
	tga[13] = static_cast<byte>((width >> 8) & 0xff);
	tga[14] = static_cast<byte>(height & 0xff);
	tga[15] = static_cast<byte>((height >> 8) & 0xff);
	tga[16] = 24; // BGR, with alpha deliberately discarded
	tga[17] = 0x20; // top-left origin

	// VRHI readback rows are ordered from the top of the 2D image; preserve
	// that order and advertise it in the TGA descriptor rather than scaling or
	// depending on alpha.
	for (size_t y = 0; y < static_cast<size_t>(height); ++y) {
		const byte *src = readback.data() + y * static_cast<size_t>(baseMip.pitch);
		byte *dst = tga.data() + 18 + y * static_cast<size_t>(width) * 3;
		for (size_t x = 0; x < static_cast<size_t>(width); ++x) {
			const byte *pixel = src + x * 4;
			if (inputBGRA) {
				dst[0] = pixel[0];
				dst[1] = pixel[1];
				dst[2] = pixel[2];
			} else {
				dst[0] = pixel[2];
				dst[1] = pixel[1];
				dst[2] = pixel[0];
			}
			dst += 3;
		}
	}

	return VRHI_WriteScreenshot(name, tga, static_cast<int>(width),
		static_cast<int>(height));
}

static void VRHI_FillConfig(glconfig_t *config) {
	if (config == nullptr) {
		return;
	}
	std::memset(config, 0, sizeof(*config));
	VRHI_CopyString(config->renderer_string, sizeof(config->renderer_string),
		g_vhDeviceInfo.name, "VRHI (Vulkan)");
	VRHI_CopyString(config->vendor_string, sizeof(config->vendor_string),
		g_vhDeviceInfo.driver, "Vulkan");
	VRHI_CopyString(config->version_string, sizeof(config->version_string),
		g_vhDeviceInfo.apiVersion, "Vulkan");
	VRHI_CopyString(config->extensions_string, sizeof(config->extensions_string),
		g_vhDeviceInfo.summary, "VRHI clear/present slice");

	config->maxTextureSize = g_vhDeviceInfo.maxTextureSize != 0
		? (int)g_vhDeviceInfo.maxTextureSize : 16384;
	config->numTextureUnits = 8;
	config->colorBits = 32;
	config->depthBits = 24;
	config->stencilBits = 8;
	config->driverType = GLDRV_ICD;
	config->hardwareType = GLHW_GENERIC;
	config->deviceSupportsGamma = qfalse;
	config->textureCompression = TC_NONE;
	config->textureEnvAddAvailable = qfalse;
	config->vidWidth = g_windowWidth;
	config->vidHeight = g_windowHeight;
	config->windowAspect = g_windowHeight > 0
		? (float)g_windowWidth / (float)g_windowHeight : 1.0f;
	config->displayFrequency = 0;
	config->isFullscreen = qfalse;
	config->stereoEnabled = qfalse;
	config->smpActive = qfalse;
}

// This shader is deliberately a solid-color UI fallback. It draws no texture
// or world content; the shader handle and texture coordinates remain ignored.
static const char *VRHI_UIVertexSource = R"(
cbuffer UIParams : register(b0, VRHI_STAGE_SPACE)
{
    float4 ui_rect;
};

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput main(uint vertexID : SV_VertexID)
{
    float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0)
    };
    float2 pixel = ui_rect.xy + corners[vertexID] * ui_rect.zw;
    VSOutput output;
    output.position = float4(pixel * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

static const char *VRHI_UIPixelSource = R"(
cbuffer UIColor : register(b1, VRHI_STAGE_SPACE)
{
    float4 ui_color;
};

[shader("pixel")]
float4 main() : SV_Target
{
    return ui_color;
}
)";

static void VRHI_DestroyUI(void) {
	if (g_uiVertexShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_uiVertexShader);
	}
	if (g_uiPixelShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_uiPixelShader);
	}
	g_uiVertexShader = VRHI_INVALID_HANDLE;
	g_uiPixelShader = VRHI_INVALID_HANDLE;
	g_uiProgram.clear();
	g_uiState = vhState();
	g_uiInitialized = false;
}

static bool VRHI_InitializeUI(void) {
	if (!g_deviceInitialized) {
		return false;
	}
	if (g_uiInitialized) {
		return true;
	}

	std::vector<uint32_t> vertexSpirv;
	std::vector<uint32_t> pixelSpirv;
	std::string error;
	if (!vhCompileShader("VRHI_UIVertex", VRHI_UIVertexSource,
		VRHI_SHADER_STAGE_VERTEX | VRHI_SHADER_SM_6_0, vertexSpirv, "main",
		{}, {}, &error)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: solid-color UI vertex shader compile failed: %s\n",
			error.c_str());
		return false;
	}
	error.clear();
	if (!vhCompileShader("VRHI_UIPixel", VRHI_UIPixelSource,
		VRHI_SHADER_STAGE_PIXEL | VRHI_SHADER_SM_6_0, pixelSpirv, "main",
		{}, {}, &error)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: solid-color UI pixel shader compile failed: %s\n",
			error.c_str());
		return false;
	}

	g_uiVertexShader = vhAllocShader();
	g_uiPixelShader = vhAllocShader();
	if (g_uiVertexShader == VRHI_INVALID_HANDLE ||
		g_uiPixelShader == VRHI_INVALID_HANDLE) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: solid-color UI shader allocation failed\n");
		// Allocation alone does not enqueue a backend resource, so there is
		// nothing to destroy on this failure path.
		g_uiVertexShader = VRHI_INVALID_HANDLE;
		g_uiPixelShader = VRHI_INVALID_HANDLE;
		return false;
	}

	const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
	vhCreateShader(g_uiVertexShader, "VRHI_UIVertex", VRHI_SHADER_STAGE_VERTEX,
		vertexSpirv, "main");
	vhCreateShader(g_uiPixelShader, "VRHI_UIPixel", VRHI_SHADER_STAGE_PIXEL,
		pixelSpirv, "main");
	vhFinish();
	const int32_t errorsAfter = g_vhErrorCounter.load(std::memory_order_relaxed);
	if (errorsAfter != errorsBefore) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: solid-color UI shader creation failed (VRHI errors %+d)\n",
			static_cast<int>(errorsAfter - errorsBefore));
		VRHI_DestroyUI();
		return false;
	}

	g_uiProgram = vhCreateGfxProgram(g_uiVertexShader, g_uiPixelShader);
	g_uiInitialized = true;
	VRHI_Printf(PRINT_ALL,
		"renderer_vrhi: solid-color UI fallback ready (texture/world rendering remains unavailable)\n");
	return true;
}

static void VRHI_BeginRegistration(glconfig_t *config) {
	int requestedWidth;
	int requestedHeight;
	VRHI_RegisterScreenshotCommand();
	g_frameBackbufferReady = false;
	g_frameBackbuffer = VRHI_INVALID_HANDLE;
	if (!VRHI_EnsureSDLVideo()) {
		return;
	}
	VRHI_GetRequestedResolution(&requestedWidth, &requestedHeight);

	if (!VRHI_CreateWindow(requestedWidth, requestedHeight) ||
		!VRHI_UpdateWindowSize(requestedWidth, requestedHeight, true) ||
		!VRHI_GetWindowHandle()) {
		return;
	}

	if (!g_inputInitialized && g_ri.IN_Init != nullptr) {
		// IN_Init requires SDL_INIT_VIDEO and must receive the SDL_Window used by
		// this renderer. It remains live across Shutdown(qfalse).
		g_ri.IN_Init(g_window);
		g_inputInitialized = true;
	}

	if (!g_deviceInitialized) {
		g_vhInit = vhInitData{};
		g_vhInit.appName = "ioquake3";
		g_vhInit.engineName = "ioquake3 VRHI";
		g_vhInit.windowHandle = reinterpret_cast<void *>(g_windowHandle);
		g_vhInit.displayHandle = nullptr;
		g_vhInit.headless = false;
		g_vhInit.resolution = glm::ivec2(g_windowWidth, g_windowHeight);
		g_vhInit.vsync = VRHI_CvarInteger("r_swapInterval", "1") != 0;
		g_vhInit.raytracing = false;
		g_vhInit.fnLogCallback = [](bool error, const std::string &message) {
			VRHI_Printf(error ? PRINT_WARNING : PRINT_ALL, "%s", message.c_str());
		};

		// vhInit is deliberately called exactly once while this renderer DLL is
		// alive. VRHI owns the device and the primary swapchain after this point.
		vhInit(false);
		if (!g_vhDevice) {
			VRHI_Fatal("renderer_vrhi: vhInit returned without a device\n");
			return;
		}
		g_deviceInitialized = true;
		VRHI_Printf(PRINT_ALL,
			"renderer_vrhi: device ready: %s (%s)\n",
			g_vhDeviceInfo.name.empty() ? "unknown" : g_vhDeviceInfo.name.c_str(),
			g_vhDeviceInfo.apiVersion.empty() ? "unknown Vulkan API"
				: g_vhDeviceInfo.apiVersion.c_str());
	} else {
		// Re-registration is a video restart, not a second vhInit. Keep the
		// existing device and refresh only the native window/swapchain dimensions.
		g_vhInit.windowHandle = reinterpret_cast<void *>(g_windowHandle);
		VRHI_RefreshSwapchain();
	}

	// Compile the tiny UI fallback only after vhInit has created a device; a
	// shader failure leaves clear/present and all no-op callbacks usable.
	VRHI_InitializeUI();
	VRHI_FillConfig(config);
}

static void VRHI_Shutdown(qboolean destroyWindow) {
	if (!destroyWindow) {
		if (g_deviceInitialized) {
			// Keep the device and SDL window alive for a subsequent registration.
			vhFinish();
		}
		g_frameState = vhState();
		g_uiState = vhState();
		g_frameViewportWidth = 0;
		g_frameViewportHeight = 0;
		g_frameBackbufferReady = false;
		g_frameBackbuffer = VRHI_INVALID_HANDLE;
		return;
	}

	// A request is an engine-side ticket, not a VRHI command.  Do not leave it
	// pointing at a destroyed swapchain; a final shutdown cancels it explicitly.
	if (g_captureRequest) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: screenshot '%s' canceled during final shutdown\n",
			g_captureName.c_str());
		g_captureRequest = false;
		g_captureName.clear();
	}
	VRHI_RemoveScreenshotCommand();

	if (g_deviceInitialized) {
		// vhShutdown also waits internally, but the explicit finish makes this
		// ordering clear and guarantees the clear command queue is drained before
		// the device or native window is torn down.
		vhFinish();
		VRHI_DestroyUI();
		vhFinish();
		vhShutdown(false);
		g_deviceInitialized = false;
	}

	if (g_inputInitialized && g_ri.IN_Shutdown != nullptr) {
		g_ri.IN_Shutdown();
	}
	g_inputInitialized = false;

	if (g_window != nullptr) {
		SDL_DestroyWindow(g_window);
		g_window = nullptr;
	}
	g_windowHandle = nullptr;
	g_frameBackbufferReady = false;
	g_frameBackbuffer = VRHI_INVALID_HANDLE;
	g_frameState = vhState();
	g_uiState = vhState();
	g_frameViewportWidth = 0;
	g_frameViewportHeight = 0;
	g_uiColor = glm::vec4(1.0f);

	if (g_sdlVideoActive || g_sdlVideoOwned) {
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}
	g_sdlVideoActive = false;
	g_sdlVideoOwned = false;
}

static void VRHI_BeginFrame(stereoFrame_t stereoFrame) {
	(void)stereoFrame;
	g_frameBackbufferReady = false;
	g_frameBackbuffer = VRHI_INVALID_HANDLE;
	if (!g_deviceInitialized) {
		return;
	}

	g_frameBackbuffer = vhGetBackbuffer();
	if (g_frameBackbuffer == VRHI_INVALID_HANDLE) {
		// Resize/minimize is a normal transient state. vhFrame will report the
		// corresponding present failure once for the completed engine frame.
		return;
	}

	const glm::uvec2 size = vhGetWindowSize();
	const int width = size.x != 0 ? (int)size.x : g_windowWidth;
	const int height = size.y != 0 ? (int)size.y : g_windowHeight;
	const glm::vec4 clearColor(0.035f, 0.055f, 0.085f, 1.0f);

	g_frameState.SetColourAttachment(0, g_frameBackbuffer)
		.SetViewRect(glm::vec4(0.0f, 0.0f, (float)width, (float)height))
		.SetViewScissor(glm::vec4(0.0f, 0.0f, (float)width, (float)height))
		.SetViewClear(VRHI_CLEAR_COLOR, clearColor);
	g_frameViewportWidth = width;
	g_frameViewportHeight = height;
	if (g_uiInitialized) {
		// Reuse the acquired backbuffer and viewport, but explicitly disable
		// depth and enable alpha blending for UI overlays.
		g_uiState = g_frameState;
		g_uiState.SetStateFlags(VRHI_STATE_WRITE_RGB | VRHI_STATE_WRITE_A |
			VRHI_STATE_BLEND_ALPHA | VRHI_STATE_CULL_NONE |
			VRHI_STATE_PT_TRIANGLES).SetProgram(g_uiProgram);
	}
	if (!vhSetState(g_frameStateId, g_frameState)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: vhSetState failed while preparing the backbuffer\n");
		return;
	}
	vhClear(g_frameStateId, VRHI_CLEAR_COLOR);
	g_frameBackbufferReady = true;
}

static void VRHI_EndFrame(int *frontEndMsec, int *backEndMsec) {
	if (frontEndMsec != nullptr) {
		*frontEndMsec = 0;
	}
	if (backEndMsec != nullptr) {
		*backEndMsec = 0;
	}
	if (!g_deviceInitialized) {
		return;
	}

	// The client may call BeginFrame twice for stereo, but EndFrame is called
	// once for the engine frame. Present exactly once here. Screenshot capture
	// is deliberately synchronous and happens before this present, outside any
	// renderer timing path.
	if (g_captureRequest) {
		if (g_frameBackbufferReady) {
			if (VRHI_CaptureBackbuffer()) {
				// Only a verified readback and write consume the ticket. Any
				// transient swapchain/readback failure retries next frame.
				g_captureRequest = false;
				g_captureName.clear();
			}
		} else {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: screenshot '%s' pending: no cleared backbuffer "
				"is available this frame (handle=0x%08x)\n",
				g_captureName.c_str(), g_frameBackbuffer);
		}
	}

	if (!vhFrame()) {
		const glm::uvec2 size = vhGetWindowSize();
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: vhFrame present/resize failed (%ux%u); "
			"window may be minimized or resized\n", size.x, size.y);
	}
	// Never let a later EndFrame reuse a framebuffer from after present.
	g_frameBackbufferReady = false;
	g_frameBackbuffer = VRHI_INVALID_HANDLE;
}

// Registration, scene, image, and textured UI resources are intentionally not
// part of this first slice. DrawStretchPic provides only the solid-color UI
// fallback above. Every callback is nevertheless populated so the client,
// cgame, and UI can safely exercise the renderer without NULL dereferences.
//
// The four registration callbacks return stable engine-local qhandles so the
// client, cgame, and UI see successful registrations (qhandle_t 0 means
// failure). The handle policy mirrors the GL renderers: each handle space is
// independent, the first handle is 1, the same name always resolves to the
// same handle, and NULL/empty names fail with 0. No image, model, or world
// data is actually loaded or kept beyond the name->handle mapping.
static qhandle_t VRHI_RegisterName(
	std::unordered_map<std::string, qhandle_t> &handles, const char *name,
	const char *kind) {
	if (name == nullptr || name[0] == '\0') {
		VRHI_Printf(PRINT_ALL, "renderer_vrhi: %s: NULL name\n", kind);
		return 0;
	}

	std::unordered_map<std::string, qhandle_t>::const_iterator it =
		handles.find(name);
	if (it != handles.end()) {
		return it->second;
	}

	// Handles start at 1 and are never released or reused, so they stay stable
	// for the lifetime of the renderer DLL.
	const qhandle_t handle = static_cast<qhandle_t>(handles.size() + 1);
	handles.emplace(name, handle);
	VRHI_Printf(PRINT_DEVELOPER,
		"renderer_vrhi: %s('%s') -> no-op handle %d\n", kind, name, handle);
	return handle;
}

static std::unordered_map<std::string, qhandle_t> g_modelHandles;
static std::unordered_map<std::string, qhandle_t> g_skinHandles;
static std::unordered_map<std::string, qhandle_t> g_shaderHandles;

static qhandle_t VRHI_RegisterModel(const char *name) {
	return VRHI_RegisterName(g_modelHandles, name, "RegisterModel");
}
static qhandle_t VRHI_RegisterSkin(const char *name) {
	return VRHI_RegisterName(g_skinHandles, name, "RegisterSkin");
}
static qhandle_t VRHI_RegisterShader(const char *name) {
	return VRHI_RegisterName(g_shaderHandles, name, "RegisterShader");
}
static qhandle_t VRHI_RegisterShaderNoMip(const char *name) {
	// RegisterShader and RegisterShaderNoMip share one handle space, mirroring
	// the GL renderers where both paths resolve through the same shader table.
	return VRHI_RegisterName(g_shaderHandles, name, "RegisterShaderNoMip");
}
static void VRHI_LoadWorld(const char *name) {
	(void)name;
}
static void VRHI_SetWorldVisData(const byte *vis) {
	(void)vis;
}
static void VRHI_EndRegistration(void) {}
static void VRHI_ClearScene(void) {}
static void VRHI_AddRefEntityToScene(const refEntity_t *entity) {
	(void)entity;
}
static void VRHI_AddPolyToScene(qhandle_t shader, int numVerts,
	const polyVert_t *verts, int num) {
	(void)shader;
	(void)numVerts;
	(void)verts;
	(void)num;
}
static int VRHI_LightForPoint(vec3_t point, vec3_t ambientLight,
	vec3_t directedLight, vec3_t lightDir) {
	(void)point;
	if (ambientLight != nullptr) {
		std::memset(ambientLight, 0, sizeof(vec3_t));
	}
	if (directedLight != nullptr) {
		std::memset(directedLight, 0, sizeof(vec3_t));
	}
	if (lightDir != nullptr) {
		std::memset(lightDir, 0, sizeof(vec3_t));
	}
	return qfalse;
}
static void VRHI_AddLightToScene(const vec3_t org, float intensity,
	float r, float g, float b) {
	(void)org;
	(void)intensity;
	(void)r;
	(void)g;
	(void)b;
}
static void VRHI_AddAdditiveLightToScene(const vec3_t org, float intensity,
	float r, float g, float b) {
	(void)org;
	(void)intensity;
	(void)r;
	(void)g;
	(void)b;
}
static void VRHI_RenderScene(const refdef_t *fd) {
	(void)fd;
}
static void VRHI_SetColor(const float *rgba) {
	if (rgba == nullptr) {
		g_uiColor = glm::vec4(1.0f);
		return;
	}
	for (int i = 0; i < 4; ++i) {
		if (!std::isfinite(rgba[i])) {
			g_uiColor = glm::vec4(1.0f);
			return;
		}
	}
	g_uiColor = glm::vec4(rgba[0], rgba[1], rgba[2], rgba[3]);
}
static void VRHI_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader) {
	(void)s1;
	(void)t1;
	(void)s2;
	(void)t2;
	(void)shader;
	if (!g_deviceInitialized || !g_uiInitialized || !g_frameBackbufferReady ||
		g_frameBackbuffer == VRHI_INVALID_HANDLE || g_frameViewportWidth <= 0 ||
		g_frameViewportHeight <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
		!std::isfinite(w) || !std::isfinite(h) || w <= 0.0f || h <= 0.0f) {
		return;
	}

	// Coordinates are in the current viewport's top-left-origin pixel space;
	// the vertex shader converts this normalized rectangle to clip space.
	const glm::vec4 rect(x / (float)g_frameViewportWidth,
		y / (float)g_frameViewportHeight,
		w / (float)g_frameViewportWidth,
		h / (float)g_frameViewportHeight);
	g_uiState.SetUniform(0, { "ui_rect", { rect } });
	g_uiState.SetUniform(1, { "ui_color", { g_uiColor } });
	if (vhSetState(g_uiStateId, g_uiState)) {
		vhDraw(g_uiStateId, 6);
	}
}
static void VRHI_DrawStretchRaw(int x, int y, int w, int h, int cols,
	int rows, const byte *data, int client, qboolean dirty) {
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)cols;
	(void)rows;
	(void)data;
	(void)client;
	(void)dirty;
}
static void VRHI_UploadCinematic(int w, int h, int cols, int rows,
	const byte *data, int client, qboolean dirty) {
	(void)w;
	(void)h;
	(void)cols;
	(void)rows;
	(void)data;
	(void)client;
	(void)dirty;
}
static int VRHI_MarkFragments(int numPoints, const vec3_t *points,
	const vec3_t projection, int maxPoints, vec3_t pointBuffer,
	int maxFragments, markFragment_t *fragmentBuffer) {
	(void)numPoints;
	(void)points;
	(void)projection;
	(void)maxPoints;
	(void)pointBuffer;
	(void)maxFragments;
	(void)fragmentBuffer;
	return 0;
}
static int VRHI_LerpTag(orientation_t *tag, qhandle_t model, int startFrame,
	int endFrame, float frac, const char *tagName) {
	(void)model;
	(void)startFrame;
	(void)endFrame;
	(void)frac;
	(void)tagName;
	if (tag != nullptr) {
		std::memset(tag, 0, sizeof(*tag));
	}
	return 0;
}
static void VRHI_ModelBounds(qhandle_t model, vec3_t mins, vec3_t maxs) {
	(void)model;
	if (mins != nullptr) {
		std::memset(mins, 0, sizeof(vec3_t));
	}
	if (maxs != nullptr) {
		std::memset(maxs, 0, sizeof(vec3_t));
	}
}
static void VRHI_RegisterFont(const char *fontName, int pointSize,
	fontInfo_t *font) {
	(void)fontName;
	(void)pointSize;
	if (font != nullptr) {
		std::memset(font, 0, sizeof(*font));
	}
}
static void VRHI_RemapShader(const char *oldShader, const char *newShader,
	const char *offsetTime) {
	(void)oldShader;
	(void)newShader;
	(void)offsetTime;
}
static qboolean VRHI_GetEntityToken(char *buffer, int size) {
	if (buffer != nullptr && size > 0) {
		buffer[0] = '\0';
	}
	return qfalse;
}
static qboolean VRHI_InPVS(const vec3_t p1, const vec3_t p2) {
	(void)p1;
	(void)p2;
	return qfalse;
}
static void VRHI_TakeVideoFrame(int h, int w, byte *captureBuffer,
	byte *encodeBuffer, qboolean motionJpeg) {
	(void)h;
	(void)w;
	(void)captureBuffer;
	(void)encodeBuffer;
	(void)motionJpeg;
}
#ifdef __USEA3D
static void VRHI_A3DRenderGeometry(void *pVoidA3D, void *pVoidGeom,
	void *pVoidMat, void *pVoidGeomStatus) {
	(void)pVoidA3D;
	(void)pVoidGeom;
	(void)pVoidMat;
	(void)pVoidGeomStatus;
}
#endif

} // namespace

extern "C" Q_EXPORT refexport_t *QDECL GetRefAPI(int apiVersion,
	refimport_t *rimp) {
	if (apiVersion != REF_API_VERSION) {
		VRHI_Diagnostic(rimp,
			"renderer_vrhi: mismatched REF_API_VERSION; refusing to load\n");
		return nullptr;
	}

	if (rimp != nullptr) {
		g_ri = *rimp;
	} else {
		std::memset(&g_ri, 0, sizeof(g_ri));
	}

	static refexport_t exports;
	std::memset(&exports, 0, sizeof(exports));

	exports.Shutdown = VRHI_Shutdown;
	exports.BeginRegistration = VRHI_BeginRegistration;
	exports.RegisterModel = VRHI_RegisterModel;
	exports.RegisterSkin = VRHI_RegisterSkin;
	exports.RegisterShader = VRHI_RegisterShader;
	exports.RegisterShaderNoMip = VRHI_RegisterShaderNoMip;
	exports.LoadWorld = VRHI_LoadWorld;
	exports.SetWorldVisData = VRHI_SetWorldVisData;
	exports.EndRegistration = VRHI_EndRegistration;
	exports.ClearScene = VRHI_ClearScene;
	exports.AddRefEntityToScene = VRHI_AddRefEntityToScene;
	exports.AddPolyToScene = VRHI_AddPolyToScene;
	exports.LightForPoint = VRHI_LightForPoint;
	exports.AddLightToScene = VRHI_AddLightToScene;
	exports.AddAdditiveLightToScene = VRHI_AddAdditiveLightToScene;
	exports.RenderScene = VRHI_RenderScene;
	exports.SetColor = VRHI_SetColor;
	exports.DrawStretchPic = VRHI_DrawStretchPic;
	exports.DrawStretchRaw = VRHI_DrawStretchRaw;
	exports.UploadCinematic = VRHI_UploadCinematic;
	exports.BeginFrame = VRHI_BeginFrame;
	exports.EndFrame = VRHI_EndFrame;
	exports.MarkFragments = VRHI_MarkFragments;
	exports.LerpTag = VRHI_LerpTag;
	exports.ModelBounds = VRHI_ModelBounds;
#ifdef __USEA3D
	exports.A3D_RenderGeometry = VRHI_A3DRenderGeometry;
#endif
	exports.RegisterFont = VRHI_RegisterFont;
	exports.RemapShader = VRHI_RemapShader;
	exports.GetEntityToken = VRHI_GetEntityToken;
	exports.inPVS = VRHI_InPVS;
	exports.TakeVideoFrame = VRHI_TakeVideoFrame;

	VRHI_Printf(PRINT_ALL,
		"renderer_vrhi: loaded (clear/present + solid-color UI fallback; "
		"textured resources/world are no-op)\n");
	return &exports;
}
