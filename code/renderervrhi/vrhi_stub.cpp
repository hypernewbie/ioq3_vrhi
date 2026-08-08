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
#include "qcommon/qfiles.h"
#include "qcommon/surfaceflags.h"
#include "renderercommon/tr_public.h"
#include "renderervrhi/vrhi_tga_decode.h"

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
static vhShader g_worldVertexShader = VRHI_INVALID_HANDLE;
static vhShader g_worldSolidPixelShader = VRHI_INVALID_HANDLE;
static vhShader g_worldLightmapPixelShader = VRHI_INVALID_HANDLE;
static vhShader g_worldDiffusePixelShader = VRHI_INVALID_HANDLE;
static vhProgram g_worldSolidProgram;
static vhProgram g_worldLightmapProgram;
static vhProgram g_worldDiffuseProgram;
static vhState g_worldState;
static const vhStateId g_worldStateId = 3;
static vhBuffer g_worldVertexBuffer = VRHI_INVALID_HANDLE;
static vhBuffer g_worldIndexBuffer = VRHI_INVALID_HANDLE;
static vhTexture g_worldDepthTexture = VRHI_INVALID_HANDLE;
static vhTexture g_worldLightmapTexture = VRHI_INVALID_HANDLE;
static nvrhi::Format g_worldDepthFormat = nvrhi::Format::UNKNOWN;
static std::vector<glm::vec3> g_worldPositions;
struct VRHI_WorldVertex {
	glm::vec3 position;
	glm::vec2 diffuse;
	glm::vec2 lightmap;
	float lightmapLayer;
};
struct VRHI_WorldDiffuseImage {
	std::string path;
	int width = 0;
	int height = 0;
	std::vector<byte> pixels;
	vhTexture texture = VRHI_INVALID_HANDLE;
};
struct VRHI_WorldBatch {
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int diffuseImage = -1;
};
// Minimal, decoded copies of the BSP PVS traversal state. Values are
// byte-swapped out of the on-disk little-endian form at load time and copied
// out of the FS buffer, so no pointer into FS data is retained. Only the
// fields needed to locate the camera leaf and map visible leaves to surface
// batches are kept; the rest of the node/leaf payload is discarded.
struct VRHI_WorldPlane {
	glm::vec3 normal;
	float dist = 0.0f;
};
struct VRHI_WorldNode {
	int32_t planeNum = 0;
	int32_t children[2] = { 0, 0 };
};
struct VRHI_WorldLeaf {
	int32_t cluster = -1;
	int32_t firstLeafSurface = 0;
	int32_t numLeafSurfaces = 0;
};
static std::vector<VRHI_WorldVertex> g_worldVertices;
static std::vector<uint32_t> g_worldIndexes;
static std::vector<VRHI_WorldBatch> g_worldBatches;
static std::vector<VRHI_WorldDiffuseImage> g_worldDiffuseImages;
static std::vector<byte> g_worldLightmapPixels;
// BSP PVS cull state (CPU copies, see VRHI_WorldPlane/Node/Leaf above).
static std::vector<VRHI_WorldPlane> g_worldPlanes;
static std::vector<VRHI_WorldNode> g_worldNodes;
static std::vector<VRHI_WorldLeaf> g_worldLeafs;
static std::vector<int32_t> g_worldLeafSurfaces;
static std::vector<int32_t> g_worldSurfaceBatch; // surface index -> batch index or -1
static std::vector<byte> g_worldVisBits;         // numClusters * clusterBytes rows
static std::vector<int32_t> g_worldBatchMarked;  // per-batch visible epoch stamp
static int32_t g_worldVisEpoch = 0;
static int32_t g_worldNumClusters = 0;
static int32_t g_worldClusterBytes = 0;
static bool g_worldVisAvailable = false;
static bool g_worldCullActive = false;
static int g_worldCameraLeaf = -1;
static int g_worldCameraCluster = -1;
static int g_worldVisibleClusters = 0;
static int g_worldVisibleBatches = 0;
static uint32_t g_worldVisibleIndexes = 0;
static bool g_worldCullReported = false;
static int g_worldLightmapLayers = 0;
static bool g_worldLightmapAvailable = false;
static bool g_worldLoaded = false;
static bool g_worldShaderInitialized = false;
static int32_t g_worldDrawErrorBaseline = 0;
static bool g_worldDrawSubmitted = false;
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

static const float VRHI_WORLD_NEAR = 4.0f;
static const float VRHI_WORLD_FAR = 131072.0f;
// Keep malformed or hostile BSP lumps from forcing an unbounded CPU/GPU
// allocation. Normal Quake 3 maps use far fewer layers.
static const int VRHI_MAX_WORLD_LIGHTMAP_LAYERS = 1024;
// Diffuse loading is intentionally a small, direct-TGA subset. Keep both the
// per-image and aggregate caps bounded when map shader names are malformed.
static const int VRHI_MAX_WORLD_DIFFUSE_IMAGES = 256;
static const int VRHI_MAX_WORLD_DIFFUSE_DIMENSION = 2048;
static const size_t VRHI_MAX_WORLD_DIFFUSE_BYTES = 64u * 1024u * 1024u;
// PVS cull state is a bounded, decoded CPU copy of the BSP node/leaf/plane/
// leafsurface/visibility lumps. Normal Quake 3 maps stay far below these caps;
// exceeding a cap disables culling (all-visible fallback) instead of allocating
// unbounded memory from a malformed or hostile lump.
static const int VRHI_MAX_WORLD_PLANES = 65536;
static const int VRHI_MAX_WORLD_NODES = 65536;
static const int VRHI_MAX_WORLD_LEAFS = 65536;
static const int VRHI_MAX_WORLD_LEAFSURFACES = 262144;
static const int VRHI_MAX_WORLD_CLUSTERS = 65536;
static const size_t VRHI_MAX_WORLD_VIS_BYTES = 16u * 1024u * 1024u;

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

static const char *VRHI_WorldVertexSource = R"(
cbuffer GlobalUniforms : register(b0, VRHI_STAGE_SPACE)
{
    float4 u_viewRect;
    float4 u_viewTexel;
    float4x4 u_view;
    float4x4 u_invView;
    float4x4 u_proj;
    float4x4 u_invProj;
    float4x4 u_viewProj;
    float4x4 u_invViewProj;
    float4 u_alphaRef4;
    float4 u_global[21];
};
cbuffer WorldUniforms : register(b1, VRHI_STAGE_SPACE)
{
    float4x4 u_world[4];
    float4x4 u_worldView;
    float4x4 u_worldViewProj;
    float4 _pad[8];
};
struct VSOutput {
    float4 position : SV_Position;
    float2 diffuse : TEXCOORD0;
    float2 lightmap : TEXCOORD1;
    float lightmapLayer : TEXCOORD2;
};
[shader("vertex")]
VSOutput main(float3 position : POSITION, float2 diffuse : TEXCOORD0,
    float2 lightmap : TEXCOORD1, float lightmapLayer : TEXCOORD2)
{
    VSOutput output;
    output.position = mul(u_worldViewProj, float4(position, 1.0));
    output.diffuse = diffuse;
    output.lightmap = lightmap;
    output.lightmapLayer = lightmapLayer;
    return output;
}
)";

static const char *VRHI_WorldSolidPixelSource = R"(
[shader("pixel")]
float4 main() : SV_Target
{
    return float4(0.24, 0.42, 0.22, 1.0);
}
)";

static const char *VRHI_WorldLightmapPixelSource = R"(
Texture2DArray<float4> u_lightmap : register(t0, VRHI_STAGE_SPACE);
SamplerState u_lightmapSampler : register(s0, VRHI_STAGE_SPACE);
struct PSInput {
    float4 position : SV_Position;
    float2 diffuse : TEXCOORD0;
    float2 lightmap : TEXCOORD1;
    float lightmapLayer : TEXCOORD2;
};
[shader("pixel")]
float4 main(PSInput input) : SV_Target
{
    const float4 solid = float4(0.24, 0.42, 0.22, 1.0);
    if (input.lightmapLayer < -0.5)
        return solid;
    return float4(u_lightmap.Sample(u_lightmapSampler,
        float3(saturate(input.lightmap), input.lightmapLayer)).rgb, 1.0);
}
)";

static const char *VRHI_WorldDiffusePixelSource = R"(
Texture2D<float4> u_diffuse : register(t0, VRHI_STAGE_SPACE);
SamplerState u_diffuseSampler : register(s0, VRHI_STAGE_SPACE);
Texture2DArray<float4> u_lightmap : register(t1, VRHI_STAGE_SPACE);
SamplerState u_lightmapSampler : register(s1, VRHI_STAGE_SPACE);
struct PSInput {
    float4 position : SV_Position;
    float2 diffuse : TEXCOORD0;
    float2 lightmap : TEXCOORD1;
    float lightmapLayer : TEXCOORD2;
};
[shader("pixel")]
float4 main(PSInput input) : SV_Target
{
    float3 color = u_diffuse.Sample(u_diffuseSampler, input.diffuse).rgb;
    if (input.lightmapLayer >= -0.5)
        color *= u_lightmap.Sample(u_lightmapSampler,
            float3(saturate(input.lightmap), input.lightmapLayer)).rgb;
    return float4(color, 1.0);
}
)";

// This shader is deliberately a solid-color UI fallback. It draws no texture
// or world content; the shader handle and texture coordinates remain ignored.
static const char *VRHI_UIVertexSource = R"(
cbuffer globalParams : register(b300, VRHI_STAGE_SPACE)
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
cbuffer globalParams : register(b300, VRHI_STAGE_SPACE)
{
    float4 ui_color;
};

[shader("pixel")]
float4 main() : SV_Target
{
    return ui_color;
}
)";

static void VRHI_DestroyWorldResources(bool clearGeometry) {
	if (g_deviceInitialized) {
		vhFinish();
	}
	if (g_worldVertexBuffer != VRHI_INVALID_HANDLE) {
		vhDestroyBuffer(g_worldVertexBuffer);
		g_worldVertexBuffer = VRHI_INVALID_HANDLE;
	}
	if (g_worldIndexBuffer != VRHI_INVALID_HANDLE) {
		vhDestroyBuffer(g_worldIndexBuffer);
		g_worldIndexBuffer = VRHI_INVALID_HANDLE;
	}
	if (g_worldDepthTexture != VRHI_INVALID_HANDLE) {
		vhDestroyTexture(g_worldDepthTexture);
		g_worldDepthTexture = VRHI_INVALID_HANDLE;
		g_worldDepthFormat = nvrhi::Format::UNKNOWN;
	}
	if (g_worldLightmapTexture != VRHI_INVALID_HANDLE) {
		vhDestroyTexture(g_worldLightmapTexture);
		g_worldLightmapTexture = VRHI_INVALID_HANDLE;
	}
	if (g_worldVertexShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_worldVertexShader);
		g_worldVertexShader = VRHI_INVALID_HANDLE;
	}
	if (g_worldSolidPixelShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_worldSolidPixelShader);
		g_worldSolidPixelShader = VRHI_INVALID_HANDLE;
	}
	if (g_worldLightmapPixelShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_worldLightmapPixelShader);
		g_worldLightmapPixelShader = VRHI_INVALID_HANDLE;
	}
	if (g_worldDiffusePixelShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_worldDiffusePixelShader);
		g_worldDiffusePixelShader = VRHI_INVALID_HANDLE;
	}
	for (VRHI_WorldDiffuseImage &image : g_worldDiffuseImages) {
		if (image.texture != VRHI_INVALID_HANDLE) {
			vhDestroyTexture(image.texture);
			image.texture = VRHI_INVALID_HANDLE;
		}
	}
	g_worldSolidProgram.clear();
	g_worldLightmapProgram.clear();
	g_worldDiffuseProgram.clear();
	g_worldLightmapAvailable = false;
	g_worldLightmapLayers = 0;
	g_worldLightmapPixels.clear();
	g_worldShaderInitialized = false;
	g_worldState = vhState();
	g_worldPlanes.clear();
	g_worldNodes.clear();
	g_worldLeafs.clear();
	g_worldLeafSurfaces.clear();
	g_worldSurfaceBatch.clear();
	g_worldVisBits.clear();
	g_worldBatchMarked.clear();
	g_worldVisEpoch = 0;
	g_worldNumClusters = 0;
	g_worldClusterBytes = 0;
	g_worldVisAvailable = false;
	g_worldCullActive = false;
	g_worldCameraLeaf = -1;
	g_worldCameraCluster = -1;
	g_worldVisibleClusters = 0;
	g_worldVisibleBatches = 0;
	g_worldVisibleIndexes = 0;
	g_worldCullReported = false;
	if (clearGeometry) {
		g_worldPositions.clear();
		g_worldVertices.clear();
		g_worldIndexes.clear();
		g_worldBatches.clear();
		g_worldDiffuseImages.clear();
		g_worldLoaded = false;
	}
	if (g_deviceInitialized) {
		vhFinish();
	}
}

static bool VRHI_InitializeWorldShader(void) {
	if (!g_deviceInitialized) return false;
	if (g_worldShaderInitialized) return true;
	std::vector<uint32_t> vertexSpirv;
	std::vector<uint32_t> solidPixelSpirv;
	std::vector<uint32_t> lightmapPixelSpirv;
	std::vector<uint32_t> diffusePixelSpirv;
	std::string error;
	if (!vhCompileShader("VRHI_WorldVertex", VRHI_WorldVertexSource,
		VRHI_SHADER_STAGE_VERTEX | VRHI_SHADER_SM_6_0, vertexSpirv, "main",
		{}, {}, &error) ||
		!vhCompileShader("VRHI_WorldSolidPixel", VRHI_WorldSolidPixelSource,
		VRHI_SHADER_STAGE_PIXEL | VRHI_SHADER_SM_6_0, solidPixelSpirv, "main",
		{}, {}, &error)) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: world shader compile failed: %s\n",
			error.c_str());
		return false;
	}
	std::string lightmapError;
	const bool lightmapCompiled = vhCompileShader("VRHI_WorldLightmapPixel",
		VRHI_WorldLightmapPixelSource, VRHI_SHADER_STAGE_PIXEL | VRHI_SHADER_SM_6_0,
		lightmapPixelSpirv, "main", {}, {}, &lightmapError);
	if (!lightmapCompiled) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: lightmap shader unavailable; using solid fallback: %s\n",
			lightmapError.c_str());
	}
	std::string diffuseError;
	const bool diffuseCompiled = vhCompileShader("VRHI_WorldDiffusePixel",
		VRHI_WorldDiffusePixelSource, VRHI_SHADER_STAGE_PIXEL | VRHI_SHADER_SM_6_0,
		diffusePixelSpirv, "main", {}, {}, &diffuseError);
	if (!diffuseCompiled) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: diffuse shader unavailable; TGA surfaces use lightmap/solid fallback: %s\n",
			diffuseError.c_str());
	}
	g_worldVertexShader = vhAllocShader();
	g_worldSolidPixelShader = vhAllocShader();
	if (lightmapCompiled) g_worldLightmapPixelShader = vhAllocShader();
	if (diffuseCompiled) g_worldDiffusePixelShader = vhAllocShader();
	if (g_worldVertexShader == VRHI_INVALID_HANDLE ||
		g_worldSolidPixelShader == VRHI_INVALID_HANDLE ||
		(lightmapCompiled && g_worldLightmapPixelShader == VRHI_INVALID_HANDLE) ||
		(diffuseCompiled && g_worldDiffusePixelShader == VRHI_INVALID_HANDLE)) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: world shader allocation failed\n");
		VRHI_DestroyWorldResources(false);
		return false;
	}
	const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
	vhCreateShader(g_worldVertexShader, "VRHI_WorldVertex", VRHI_SHADER_STAGE_VERTEX,
		vertexSpirv, "main");
	vhCreateShader(g_worldSolidPixelShader, "VRHI_WorldSolidPixel", VRHI_SHADER_STAGE_PIXEL,
		solidPixelSpirv, "main");
	if (lightmapCompiled) {
		vhCreateShader(g_worldLightmapPixelShader, "VRHI_WorldLightmapPixel",
			VRHI_SHADER_STAGE_PIXEL, lightmapPixelSpirv, "main");
	}
	if (diffuseCompiled) {
		vhCreateShader(g_worldDiffusePixelShader, "VRHI_WorldDiffusePixel",
			VRHI_SHADER_STAGE_PIXEL, diffusePixelSpirv, "main");
	}
	vhFinish();
	if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: world shader creation failed\n");
		VRHI_DestroyWorldResources(false);
		return false;
	}
	g_worldSolidProgram = vhCreateGfxProgram(g_worldVertexShader, g_worldSolidPixelShader);
	if (lightmapCompiled) {
		g_worldLightmapProgram = vhCreateGfxProgram(g_worldVertexShader,
			g_worldLightmapPixelShader);
	}
	if (diffuseCompiled) {
		g_worldDiffuseProgram = vhCreateGfxProgram(g_worldVertexShader,
			g_worldDiffusePixelShader);
	}
	g_worldShaderInitialized = true;
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: static BSP world shaders ready (lightmap=%s diffuse=%s)\n",
		lightmapCompiled ? "yes" : "no", diffuseCompiled ? "yes" : "no");
	return true;
}

static bool VRHI_CreateWorldDepth(int width, int height) {
	if (!g_deviceInitialized || width <= 0 || height <= 0) return false;
	if (g_worldDepthTexture != VRHI_INVALID_HANDLE) {
		const vhTexInfo info = vhGetTextureInfo(g_worldDepthTexture);
		if (info.dimensions.x == width && info.dimensions.y == height) return true;
		vhFinish();
		vhDestroyTexture(g_worldDepthTexture);
		g_worldDepthTexture = VRHI_INVALID_HANDLE;
		g_worldDepthFormat = nvrhi::Format::UNKNOWN;
	}
	const nvrhi::Format formats[] = { nvrhi::Format::D32S8,
		nvrhi::Format::D24S8, nvrhi::Format::D32, nvrhi::Format::D16 };
	for (nvrhi::Format format : formats) {
		vhTexture texture = vhAllocTexture();
		if (texture == VRHI_INVALID_HANDLE) continue;
		const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
		vhCreateTexture2D(texture, "VRHI_WorldDepth", glm::ivec2(width, height), 1,
			format, VRHI_TEXTURE_RT);
		vhFinish();
		if (g_vhErrorCounter.load(std::memory_order_relaxed) == errorsBefore) {
			g_worldDepthTexture = texture;
			g_worldDepthFormat = format;
			VRHI_Printf(PRINT_ALL, "renderer_vrhi: world depth %dx%d format %s\n",
				width, height, VRHI_TextureFormatName(format));
			return true;
		}
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: depth format %s unavailable; trying fallback\n",
			VRHI_TextureFormatName(format));
		vhDestroyTexture(texture);
		vhFinish();
	}
	VRHI_Printf(PRINT_WARNING, "renderer_vrhi: no supported world depth format\n");
	return false;
}

static bool VRHI_UploadWorldLightmaps(void) {
	if (!g_deviceInitialized || g_worldLightmapTexture != VRHI_INVALID_HANDLE ||
		g_worldLightmapLayers <= 0 || g_worldLightmapPixels.empty() ||
		g_worldLightmapPixelShader == VRHI_INVALID_HANDLE) return g_worldLightmapAvailable;
	const size_t layerBytes = static_cast<size_t>(LIGHTMAP_WIDTH) * LIGHTMAP_HEIGHT * 4;
	if (g_worldLightmapPixels.size() != layerBytes * static_cast<size_t>(g_worldLightmapLayers)) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: invalid decoded lightmap storage\n");
		return false;
	}
	vhTexture texture = vhAllocTexture();
	if (texture == VRHI_INVALID_HANDLE) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: lightmap texture allocation failed; using solid world\n");
		return false;
	}
	vhMem *data = new vhMem(g_worldLightmapPixels.size());
	std::memcpy(data->data(), g_worldLightmapPixels.data(), data->size());
	const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
	vhCreateTexture2DArray(texture, "VRHI_BSPLightmaps",
		glm::ivec2(LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT), g_worldLightmapLayers, 1,
		nvrhi::Format::RGBA8_UNORM, VRHI_TEXTURE_NONE | VRHI_SAMPLER_NONE, data);
	vhFinish();
	if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: lightmap texture upload failed; using solid world\n");
		vhDestroyTexture(texture);
		vhFinish();
		return false;
	}
	g_worldLightmapTexture = texture;
	g_worldLightmapAvailable = true;
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: uploaded BSP lightmaps (%d layers, %zu bytes)\n",
		g_worldLightmapLayers, g_worldLightmapPixels.size());
	return true;
}

static bool VRHI_UploadWorldDiffuse(void) {
	if (!g_deviceInitialized || g_worldDiffusePixelShader == VRHI_INVALID_HANDLE) return false;
	bool anyUploaded = false;
	for (VRHI_WorldDiffuseImage &image : g_worldDiffuseImages) {
		if (image.texture != VRHI_INVALID_HANDLE) {
			anyUploaded = true;
			continue;
		}
		if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) continue;
		vhTexture texture = vhAllocTexture();
		if (texture == VRHI_INVALID_HANDLE) {
			VRHI_Printf(PRINT_WARNING, "renderer_vrhi: diffuse TGA '%s' allocation failed; using lightmap/solid fallback\n", image.path.c_str());
			continue;
		}
		vhMem *data = new vhMem(image.pixels.size());
		std::memcpy(data->data(), image.pixels.data(), data->size());
		const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
		vhCreateTexture2D(texture, image.path.c_str(), glm::ivec2(image.width, image.height), 1,
			nvrhi::Format::RGBA8_UNORM, VRHI_TEXTURE_NONE | VRHI_SAMPLER_NONE, data);
		vhFinish();
		if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
			VRHI_Printf(PRINT_WARNING, "renderer_vrhi: diffuse TGA '%s' upload failed; using lightmap/solid fallback\n", image.path.c_str());
			vhDestroyTexture(texture);
			vhFinish();
			continue;
		}
		image.texture = texture;
		anyUploaded = true;
		VRHI_Printf(PRINT_ALL, "renderer_vrhi: uploaded BSP diffuse '%s' (%dx%d, %zu bytes)\n",
			image.path.c_str(), image.width, image.height, image.pixels.size());
	}
	return anyUploaded;
}

static bool VRHI_UploadWorldGeometry(void) {
	if (!g_deviceInitialized || !g_worldLoaded || g_worldVertices.empty() ||
		g_worldIndexes.empty()) return false;
	if (!VRHI_InitializeWorldShader()) return false;
	if (g_worldVertexBuffer != VRHI_INVALID_HANDLE || g_worldIndexBuffer != VRHI_INVALID_HANDLE) vhFinish();
	if (g_worldVertexBuffer != VRHI_INVALID_HANDLE) vhDestroyBuffer(g_worldVertexBuffer);
	if (g_worldIndexBuffer != VRHI_INVALID_HANDLE) vhDestroyBuffer(g_worldIndexBuffer);
	g_worldVertexBuffer = vhAllocBuffer();
	g_worldIndexBuffer = vhAllocBuffer();
	if (g_worldVertexBuffer == VRHI_INVALID_HANDLE || g_worldIndexBuffer == VRHI_INVALID_HANDLE) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: world buffer allocation failed\n");
		if (g_worldVertexBuffer != VRHI_INVALID_HANDLE) vhDestroyBuffer(g_worldVertexBuffer);
		if (g_worldIndexBuffer != VRHI_INVALID_HANDLE) vhDestroyBuffer(g_worldIndexBuffer);
		g_worldVertexBuffer = g_worldIndexBuffer = VRHI_INVALID_HANDLE;
		return false;
	}
	vhMem *vertices = new vhMem(g_worldVertices.size() * sizeof(VRHI_WorldVertex));
	std::memcpy(vertices->data(), g_worldVertices.data(), vertices->size());
	vhMem *indexes = new vhMem(g_worldIndexes.size() * sizeof(uint32_t));
	std::memcpy(indexes->data(), g_worldIndexes.data(), indexes->size());
	const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
	vhCreateVertexBuffer(g_worldVertexBuffer, "VRHI_WorldVertices", vertices,
		"float3 float2 float2 float", g_worldVertices.size());
	vhCreateIndexBuffer(g_worldIndexBuffer, "VRHI_WorldIndexes", indexes,
		g_worldIndexes.size(), VRHI_BUFFER_INDEX32);
	vhFinish();
	if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: world buffer upload reported VRHI errors\n");
		vhDestroyBuffer(g_worldVertexBuffer);
		vhDestroyBuffer(g_worldIndexBuffer);
		g_worldVertexBuffer = g_worldIndexBuffer = VRHI_INVALID_HANDLE;
		vhFinish();
		return false;
	}
	VRHI_UploadWorldLightmaps();
	VRHI_UploadWorldDiffuse();
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: uploaded world geometry (%zu vertices, %zu indexes)\n",
		g_worldVertices.size(), g_worldIndexes.size());
	return true;
}

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
		// Release whichever IDs were allocated. No create command has been
		// submitted yet, so this cannot destroy a backend shader resource.
		if (g_uiVertexShader != VRHI_INVALID_HANDLE) {
			vhDestroyShader(g_uiVertexShader);
		}
		if (g_uiPixelShader != VRHI_INVALID_HANDLE) {
			vhDestroyShader(g_uiPixelShader);
		}
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
	VRHI_InitializeWorldShader();
	if (g_worldLoaded) VRHI_UploadWorldGeometry();
	VRHI_CreateWorldDepth(g_windowWidth, g_windowHeight);
	VRHI_FillConfig(config);
}

static void VRHI_Shutdown(qboolean destroyWindow) {
	if (!destroyWindow) {
		if (g_deviceInitialized) {
			// Keep the device and SDL window alive for a subsequent registration,
			// but release map/video resources before the swapchain is reused.
			VRHI_DestroyWorldResources(true);
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
		VRHI_DestroyWorldResources(true);
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
	// Depth is a renderer-owned attachment, not part of the clear-only frame
	// state. Recreate it lazily so resize/minimize transitions remain harmless.
	VRHI_CreateWorldDepth(width, height);
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
	if (g_worldDrawSubmitted) {
		const int32_t errors = g_vhErrorCounter.load(std::memory_order_relaxed) -
			g_worldDrawErrorBaseline;
		if (errors > 0) {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: world draw reported %d VRHI error(s)\n", errors);
		}
		g_worldDrawSubmitted = false;
	}
	// Never let a later EndFrame reuse a framebuffer from after present.
	g_frameBackbufferReady = false;
	g_frameBackbuffer = VRHI_INVALID_HANDLE;
}

// Entity, patch, and textured UI resources remain outside this slice.
// DrawStretchPic provides the existing solid-color UI fallback while the first
// BSP model and its bounded direct-TGA diffuse batches are rendered by the
// static world path above. Every callback is
// nevertheless populated so the client, cgame, and UI can safely exercise the
// renderer without NULL dereferences.
//
// The four registration callbacks return stable engine-local qhandles so the
// client, cgame, and UI see successful registrations (qhandle_t 0 means
// failure). The handle policy mirrors the GL renderers: each handle space is
// independent, the first handle is 1, the same name always resolves to the
// same handle, and NULL/empty names fail with 0. Registration callbacks keep
// only this name->handle mapping; BSP diffuse image data is owned separately by
// the bounded world loader.
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
static bool VRHI_ValidLump(const lump_t &lump, size_t fileSize,
	const char *name) {
	// Check the subtraction only after proving that the offset is in range;
	// this keeps malformed signed on-disk values from wrapping.
	if (lump.fileofs < 0 || lump.filelen < 0 ||
		static_cast<size_t>(lump.fileofs) > fileSize ||
		static_cast<size_t>(lump.filelen) >
			fileSize - static_cast<size_t>(lump.fileofs)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: BSP lump %s invalid (offset=%d length=%d file=%zu)\n",
			name, lump.fileofs, lump.filelen, fileSize);
		return false;
	}
	return true;
}

static dmodel_t VRHI_DecodeModel(const dmodel_t &disk) {
	dmodel_t host = disk;
	for (int i = 0; i < 3; ++i) {
		host.mins[i] = LittleFloat(disk.mins[i]);
		host.maxs[i] = LittleFloat(disk.maxs[i]);
	}
	host.firstSurface = LittleLong(disk.firstSurface);
	host.numSurfaces = LittleLong(disk.numSurfaces);
	host.firstBrush = LittleLong(disk.firstBrush);
	host.numBrushes = LittleLong(disk.numBrushes);
	return host;
}

static dsurface_t VRHI_DecodeSurface(const dsurface_t &disk) {
	dsurface_t host = disk;
	host.shaderNum = LittleLong(disk.shaderNum);
	host.fogNum = LittleLong(disk.fogNum);
	host.surfaceType = LittleLong(disk.surfaceType);
	host.firstVert = LittleLong(disk.firstVert);
	host.numVerts = LittleLong(disk.numVerts);
	host.firstIndex = LittleLong(disk.firstIndex);
	host.numIndexes = LittleLong(disk.numIndexes);
	host.lightmapNum = LittleLong(disk.lightmapNum);
	host.lightmapX = LittleLong(disk.lightmapX);
	host.lightmapY = LittleLong(disk.lightmapY);
	host.lightmapWidth = LittleLong(disk.lightmapWidth);
	host.lightmapHeight = LittleLong(disk.lightmapHeight);
	for (int i = 0; i < 3; ++i) {
		host.lightmapOrigin[i] = LittleFloat(disk.lightmapOrigin[i]);
		for (int j = 0; j < 3; ++j) {
			host.lightmapVecs[i][j] = LittleFloat(disk.lightmapVecs[i][j]);
		}
	}
	host.patchWidth = LittleLong(disk.patchWidth);
	host.patchHeight = LittleLong(disk.patchHeight);
	return host;
}

static dshader_t VRHI_DecodeShader(const dshader_t &disk) {
	dshader_t host = disk;
	host.surfaceFlags = LittleLong(disk.surfaceFlags);
	host.contentFlags = LittleLong(disk.contentFlags);
	return host;
}

static drawVert_t VRHI_DecodeDrawVert(const drawVert_t &disk) {
	drawVert_t host = disk;
	for (int i = 0; i < 3; ++i) {
		host.xyz[i] = LittleFloat(disk.xyz[i]);
		host.normal[i] = LittleFloat(disk.normal[i]);
	}
	for (int i = 0; i < 2; ++i) {
		host.st[i] = LittleFloat(disk.st[i]);
		host.lightmap[i] = LittleFloat(disk.lightmap[i]);
	}
	return host;
}

static dmodel_t VRHI_ReadModel(const byte *lumpData, int index) {
	dmodel_t disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return VRHI_DecodeModel(disk);
}

static dsurface_t VRHI_ReadSurface(const byte *lumpData, int index) {
	dsurface_t disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return VRHI_DecodeSurface(disk);
}

static dshader_t VRHI_ReadShader(const byte *lumpData, int index) {
	dshader_t disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return VRHI_DecodeShader(disk);
}

static drawVert_t VRHI_ReadDrawVert(const byte *lumpData, int index) {
	drawVert_t disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return VRHI_DecodeDrawVert(disk);
}

static int VRHI_ReadIndex(const byte *lumpData, int index) {
	int disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return LittleLong(disk);
}

// BSP PVS cull lumps are decoded with the same little-endian discipline as
// the geometry lumps above. Only the fields the point-in-BSP walk and the
// leaf-surface mapping need are kept in the CPU copies.
static dplane_t VRHI_DecodePlane(const dplane_t &disk) {
	dplane_t host = disk;
	for (int i = 0; i < 3; ++i) {
		host.normal[i] = LittleFloat(disk.normal[i]);
	}
	host.dist = LittleFloat(disk.dist);
	return host;
}

static dnode_t VRHI_DecodeNode(const dnode_t &disk) {
	dnode_t host = disk;
	host.planeNum = LittleLong(disk.planeNum);
	for (int i = 0; i < 2; ++i) {
		host.children[i] = LittleLong(disk.children[i]);
	}
	for (int i = 0; i < 3; ++i) {
		host.mins[i] = LittleLong(disk.mins[i]);
		host.maxs[i] = LittleLong(disk.maxs[i]);
	}
	return host;
}

static dleaf_t VRHI_DecodeLeaf(const dleaf_t &disk) {
	dleaf_t host = disk;
	host.cluster = LittleLong(disk.cluster);
	host.area = LittleLong(disk.area);
	for (int i = 0; i < 3; ++i) {
		host.mins[i] = LittleLong(disk.mins[i]);
		host.maxs[i] = LittleLong(disk.maxs[i]);
	}
	host.firstLeafSurface = LittleLong(disk.firstLeafSurface);
	host.numLeafSurfaces = LittleLong(disk.numLeafSurfaces);
	host.firstLeafBrush = LittleLong(disk.firstLeafBrush);
	host.numLeafBrushes = LittleLong(disk.numLeafBrushes);
	return host;
}

static dplane_t VRHI_ReadPlane(const byte *lumpData, int index) {
	dplane_t disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return VRHI_DecodePlane(disk);
}

static dnode_t VRHI_ReadNode(const byte *lumpData, int index) {
	dnode_t disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return VRHI_DecodeNode(disk);
}

static dleaf_t VRHI_ReadLeaf(const byte *lumpData, int index) {
	dleaf_t disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return VRHI_DecodeLeaf(disk);
}

static int VRHI_ReadLeafSurface(const byte *lumpData, int index) {
	int disk;
	std::memcpy(&disk, lumpData + static_cast<size_t>(index) * sizeof(disk), sizeof(disk));
	return LittleLong(disk);
}

static bool VRHI_LoadDiffuseTGA(const char *shaderName, int *imageIndex) {
	if (imageIndex != nullptr) *imageIndex = -1;
	if (shaderName == nullptr || g_ri.FS_ReadFile == nullptr) return false;
	size_t shaderLength = 0;
	while (shaderLength < MAX_QPATH && shaderName[shaderLength] != '\0') ++shaderLength;
	if (shaderLength == 0 || shaderLength >= MAX_QPATH ||
		shaderName[0] == '/' || shaderName[0] == '\\') return false;
	std::string path(shaderName, shaderLength);
	if (path.find("..") != std::string::npos) return false;
	const size_t extension = path.size() >= 4 ? path.size() - 4 : 0;
	if (path.size() < 4 || (path[extension] != '.' ||
		(path[extension + 1] != 't' && path[extension + 1] != 'T') ||
		(path[extension + 2] != 'g' && path[extension + 2] != 'G') ||
		(path[extension + 3] != 'a' && path[extension + 3] != 'A'))) {
		if (path.size() + 4 >= MAX_QPATH) return false;
		path += ".tga";
	}
	for (size_t i = 0; i < g_worldDiffuseImages.size(); ++i) {
		if (g_worldDiffuseImages[i].path == path) {
			if (imageIndex != nullptr) *imageIndex = static_cast<int>(i);
			return true;
		}
	}
	if (g_worldDiffuseImages.size() >= VRHI_MAX_WORLD_DIFFUSE_IMAGES) return false;
	void *fileData = nullptr;
	const long fileSizeLong = g_ri.FS_ReadFile(path.c_str(), &fileData);
	if (fileData == nullptr || fileSizeLong < 18) {
		if (fileData != nullptr && g_ri.FS_FreeFile != nullptr) g_ri.FS_FreeFile(fileData);
		return false;
	}
	const size_t fileSize = static_cast<size_t>(fileSizeLong);
	const byte *bytes = static_cast<const byte *>(fileData);
	// Bounded decode of uncompressed (type 2) and RLE (type 10) 24/32-bit
	// true-color TGAs. Any malformed, oversized, or unsupported file returns
	// false here and keeps the existing lightmap/solid fallback.
	vrhi_tga::DecodeResult decoded;
	if (!vrhi_tga::Decode(bytes, fileSize, VRHI_MAX_WORLD_DIFFUSE_DIMENSION,
		VRHI_MAX_WORLD_DIFFUSE_BYTES, &decoded)) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: diffuse '%s' unsupported; expected type 2/10 (RLE) 24/32-bit TGA within caps\n",
			path.c_str());
		g_ri.FS_FreeFile(fileData);
		return false;
	}
	const size_t pixelBytes = decoded.rgba.size();
	// Keep the aggregate cap independent of the number of shader references.
	size_t existingBytes = 0;
	for (const VRHI_WorldDiffuseImage &image : g_worldDiffuseImages) existingBytes += image.pixels.size();
	if (pixelBytes > VRHI_MAX_WORLD_DIFFUSE_BYTES - existingBytes) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: diffuse '%s' skipped; aggregate TGA memory cap reached\n", path.c_str());
		g_ri.FS_FreeFile(fileData);
		return false;
	}
	VRHI_WorldDiffuseImage image;
	image.path = path;
	image.width = decoded.width;
	image.height = decoded.height;
	image.pixels = std::move(decoded.rgba);
	g_ri.FS_FreeFile(fileData);
	g_worldDiffuseImages.push_back(std::move(image));
	const int index = static_cast<int>(g_worldDiffuseImages.size() - 1);
	if (imageIndex != nullptr) *imageIndex = index;
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: decoded BSP diffuse '%s' (%dx%d, %zu bytes)\n",
		path.c_str(), decoded.width, decoded.height, pixelBytes);
	return true;
}

static void VRHI_LoadWorld(const char *name) {
	VRHI_DestroyWorldResources(true);
	if (name == nullptr || name[0] == '\0' || g_ri.FS_ReadFile == nullptr) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: BSP world load missing name or filesystem\n");
		return;
	}
	void *fileData = nullptr;
	const long fileSizeLong = g_ri.FS_ReadFile(name, &fileData);
	if (fileSizeLong < static_cast<long>(sizeof(dheader_t)) || fileData == nullptr) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: BSP world '%s' read failed (%ld bytes)\n",
			name, fileSizeLong);
		return;
	}
	const size_t fileSize = static_cast<size_t>(fileSizeLong);
	const byte *fileBytes = static_cast<const byte *>(fileData);
	dheader_t diskHeader;
	std::memcpy(&diskHeader, fileBytes, sizeof(diskHeader));
	const int ident = LittleLong(diskHeader.ident);
	const int version = LittleLong(diskHeader.version);
	if (ident != BSP_IDENT || version != BSP_VERSION) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: BSP world '%s' rejected (ident=0x%08x version=%d)\n",
			name, ident, version);
		g_ri.FS_FreeFile(fileData);
		return;
	}
	lump_t lumps[HEADER_LUMPS];
	for (int i = 0; i < HEADER_LUMPS; ++i) {
		lumps[i].fileofs = LittleLong(diskHeader.lumps[i].fileofs);
		lumps[i].filelen = LittleLong(diskHeader.lumps[i].filelen);
	}
	static const char *lumpNames[HEADER_LUMPS] = { "entities", "shaders", "planes", "nodes",
		"leafs", "leafsurfaces", "leafbrushes", "models", "brushes", "brushsides",
		"drawverts", "drawindexes", "fogs", "surfaces", "lightmaps", "lightgrid", "visibility" };
	for (int i = 0; i < HEADER_LUMPS; ++i) {
		if (!VRHI_ValidLump(lumps[i], fileSize, lumpNames[i])) {
			g_ri.FS_FreeFile(fileData);
			return;
		}
	}
	const lump_t &modelsLump = lumps[LUMP_MODELS];
	const lump_t &surfacesLump = lumps[LUMP_SURFACES];
	const lump_t &vertsLump = lumps[LUMP_DRAWVERTS];
	const lump_t &indexesLump = lumps[LUMP_DRAWINDEXES];
	const lump_t &shadersLump = lumps[LUMP_SHADERS];
	const lump_t &lightmapsLump = lumps[LUMP_LIGHTMAPS];
	if (static_cast<size_t>(modelsLump.filelen) < sizeof(dmodel_t) ||
		static_cast<size_t>(shadersLump.filelen) % sizeof(dshader_t) != 0 ||
		static_cast<size_t>(surfacesLump.filelen) % sizeof(dsurface_t) != 0 ||
		static_cast<size_t>(vertsLump.filelen) % sizeof(drawVert_t) != 0 ||
		static_cast<size_t>(indexesLump.filelen) % sizeof(int) != 0) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: BSP world '%s' has malformed geometry lumps\n", name);
		g_ri.FS_FreeFile(fileData);
		return;
	}
	// ---- BSP PVS cull state (planes/nodes/leafs/leafsurfaces/visibility) ----
	// All values are decoded with LittleLong/LittleFloat and copied out of the
	// FS buffer, so nothing below retains a pointer into FS data. A missing or
	// malformed cull lump only disables culling; the world still renders with
	// the all-visible fallback, preserving the previous visual scope.
	const lump_t &planesLump = lumps[LUMP_PLANES];
	const lump_t &nodesLump = lumps[LUMP_NODES];
	const lump_t &leafsLump = lumps[LUMP_LEAFS];
	const lump_t &leafSurfacesLump = lumps[LUMP_LEAFSURFACES];
	const lump_t &visLump = lumps[LUMP_VISIBILITY];
	const int planeCount = planesLump.filelen / static_cast<int>(sizeof(dplane_t));
	const int nodeCount = nodesLump.filelen / static_cast<int>(sizeof(dnode_t));
	const int leafCount = leafsLump.filelen / static_cast<int>(sizeof(dleaf_t));
	const int leafSurfaceCount = leafSurfacesLump.filelen / static_cast<int>(sizeof(int));
	const bool cullLumpsValid = planesLump.filelen > 0 && nodesLump.filelen > 0 &&
		leafsLump.filelen > 0 && leafSurfacesLump.filelen > 0 &&
		static_cast<size_t>(planesLump.filelen) % sizeof(dplane_t) == 0 &&
		static_cast<size_t>(nodesLump.filelen) % sizeof(dnode_t) == 0 &&
		static_cast<size_t>(leafsLump.filelen) % sizeof(dleaf_t) == 0 &&
		static_cast<size_t>(leafSurfacesLump.filelen) % sizeof(int) == 0 &&
		planeCount <= VRHI_MAX_WORLD_PLANES && nodeCount <= VRHI_MAX_WORLD_NODES &&
		leafCount <= VRHI_MAX_WORLD_LEAFS && leafSurfaceCount <= VRHI_MAX_WORLD_LEAFSURFACES;
	if (!cullLumpsValid) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: BSP world '%s' cull lumps absent or oversized; all-visible fallback\n",
			name);
	}
	if (cullLumpsValid) {
		const byte *planesData = fileBytes + static_cast<size_t>(planesLump.fileofs);
		const byte *nodesData = fileBytes + static_cast<size_t>(nodesLump.fileofs);
		const byte *leafsData = fileBytes + static_cast<size_t>(leafsLump.fileofs);
		const byte *leafSurfacesData = fileBytes + static_cast<size_t>(leafSurfacesLump.fileofs);
		g_worldPlanes.reserve(static_cast<size_t>(planeCount));
		g_worldNodes.reserve(static_cast<size_t>(nodeCount));
		g_worldLeafs.reserve(static_cast<size_t>(leafCount));
		g_worldLeafSurfaces.reserve(static_cast<size_t>(leafSurfaceCount));
		for (int i = 0; i < planeCount; ++i) {
			const dplane_t plane = VRHI_ReadPlane(planesData, i);
			VRHI_WorldPlane host;
			host.normal = glm::vec3(plane.normal[0], plane.normal[1], plane.normal[2]);
			host.dist = plane.dist;
			g_worldPlanes.push_back(host);
		}
		for (int i = 0; i < nodeCount; ++i) {
			const dnode_t node = VRHI_ReadNode(nodesData, i);
			VRHI_WorldNode host;
			host.planeNum = node.planeNum;
			host.children[0] = node.children[0];
			host.children[1] = node.children[1];
			g_worldNodes.push_back(host);
		}
		for (int i = 0; i < leafCount; ++i) {
			const dleaf_t leaf = VRHI_ReadLeaf(leafsData, i);
			VRHI_WorldLeaf host;
			host.cluster = leaf.cluster;
			host.firstLeafSurface = leaf.firstLeafSurface;
			host.numLeafSurfaces = leaf.numLeafSurfaces;
			g_worldLeafs.push_back(host);
		}
		for (int i = 0; i < leafSurfaceCount; ++i) {
			g_worldLeafSurfaces.push_back(VRHI_ReadLeafSurface(leafSurfacesData, i));
		}
	}
	g_worldVisAvailable = false;
	g_worldNumClusters = 0;
	g_worldClusterBytes = 0;
	g_worldVisBits.clear();
	if (cullLumpsValid && visLump.filelen >= 8) {
		int diskNumClusters = 0;
		int diskClusterBytes = 0;
		std::memcpy(&diskNumClusters, fileBytes + static_cast<size_t>(visLump.fileofs), sizeof(diskNumClusters));
		std::memcpy(&diskClusterBytes, fileBytes + static_cast<size_t>(visLump.fileofs) + sizeof(diskNumClusters), sizeof(diskClusterBytes));
		const int numClusters = LittleLong(diskNumClusters);
		const int clusterBytes = LittleLong(diskClusterBytes);
		const bool visRowSufficient = numClusters > 0 && clusterBytes > 0 &&
			numClusters <= VRHI_MAX_WORLD_CLUSTERS &&
			clusterBytes >= (numClusters + 7) / 8 &&
			static_cast<size_t>(clusterBytes) <= VRHI_MAX_WORLD_VIS_BYTES;
		const size_t visBytes = visRowSufficient
			? static_cast<size_t>(numClusters) * static_cast<size_t>(clusterBytes) : 0;
		if (visRowSufficient && visBytes <= VRHI_MAX_WORLD_VIS_BYTES &&
			visBytes <= static_cast<size_t>(visLump.filelen) - 8) {
			g_worldNumClusters = numClusters;
			g_worldClusterBytes = clusterBytes;
			g_worldVisBits.assign(fileBytes + static_cast<size_t>(visLump.fileofs) + 8,
				fileBytes + static_cast<size_t>(visLump.fileofs) + 8 + visBytes);
			g_worldVisAvailable = true;
		} else {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: BSP world '%s' visibility lump malformed; all-visible fallback\n",
				name);
		}
	}
	const byte *modelsData = fileBytes + static_cast<size_t>(modelsLump.fileofs);
	const byte *surfacesData = fileBytes + static_cast<size_t>(surfacesLump.fileofs);
	const byte *shadersData = fileBytes + static_cast<size_t>(shadersLump.fileofs);
	const byte *vertsData = fileBytes + static_cast<size_t>(vertsLump.fileofs);
	const byte *indexesData = fileBytes + static_cast<size_t>(indexesLump.fileofs);
	const int surfaceCount = surfacesLump.filelen / static_cast<int>(sizeof(dsurface_t));
	const int vertCount = vertsLump.filelen / static_cast<int>(sizeof(drawVert_t));
	const int indexCount = indexesLump.filelen / static_cast<int>(sizeof(int));
	const int shaderCount = shadersLump.filelen / static_cast<int>(sizeof(dshader_t));
	const size_t lightmapLayerBytes = static_cast<size_t>(LIGHTMAP_WIDTH) *
		LIGHTMAP_HEIGHT * 3;
	const bool lightmapLumpValid = lightmapsLump.filelen > 0 &&
		static_cast<size_t>(lightmapsLump.filelen) % lightmapLayerBytes == 0 &&
		static_cast<size_t>(lightmapsLump.filelen) / lightmapLayerBytes <=
		static_cast<size_t>(VRHI_MAX_WORLD_LIGHTMAP_LAYERS);
	if (lightmapsLump.filelen > 0 && !lightmapLumpValid) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: BSP world '%s' has malformed lightmap lump; surfaces use solid fallback\n",
			name);
	}
	if (lightmapLumpValid) {
		g_worldLightmapLayers = lightmapsLump.filelen / static_cast<int>(lightmapLayerBytes);
		g_worldLightmapPixels.resize(static_cast<size_t>(g_worldLightmapLayers) *
			static_cast<size_t>(LIGHTMAP_WIDTH) * LIGHTMAP_HEIGHT * 4);
		const byte *lightmapData = fileBytes + static_cast<size_t>(lightmapsLump.fileofs);
		for (int layer = 0; layer < g_worldLightmapLayers; ++layer) {
			const byte *src = lightmapData + static_cast<size_t>(layer) * lightmapLayerBytes;
			byte *dst = g_worldLightmapPixels.data() + static_cast<size_t>(layer) *
				static_cast<size_t>(LIGHTMAP_WIDTH) * LIGHTMAP_HEIGHT * 4;
			for (size_t pixel = 0; pixel < static_cast<size_t>(LIGHTMAP_WIDTH) * LIGHTMAP_HEIGHT; ++pixel) {
				dst[pixel * 4 + 0] = src[pixel * 3 + 0];
				dst[pixel * 4 + 1] = src[pixel * 3 + 1];
				dst[pixel * 4 + 2] = src[pixel * 3 + 2];
				dst[pixel * 4 + 3] = 255;
			}
		}
	}
	const dmodel_t model = VRHI_ReadModel(modelsData, 0);
	if (model.firstSurface < 0 || model.numSurfaces < 0 ||
		model.firstSurface > surfaceCount || model.numSurfaces > surfaceCount - model.firstSurface) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: BSP world '%s' has invalid first model surface range\n", name);
		g_ri.FS_FreeFile(fileData);
		return;
	}
	int accepted = 0;
	int skipped = 0;
	// Map every surface in the surfaces lump to its batch index so the
	// leafsurface lump can be resolved at render time. Entries that were not
	// accepted (or belong to submodels) stay -1 and are never drawn.
	g_worldSurfaceBatch.assign(static_cast<size_t>(surfaceCount), -1);
	for (int surfaceIndex = model.firstSurface;
		surfaceIndex < model.firstSurface + model.numSurfaces; ++surfaceIndex) {
		const dsurface_t surface = VRHI_ReadSurface(surfacesData, surfaceIndex);
		const dshader_t shader = surface.shaderNum >= 0 && surface.shaderNum < shaderCount
			? VRHI_ReadShader(shadersData, surface.shaderNum) : dshader_t();
		if ((surface.surfaceType != MST_PLANAR && surface.surfaceType != MST_TRIANGLE_SOUP) ||
			surface.shaderNum < 0 || surface.shaderNum >= shaderCount ||
			(shader.surfaceFlags & (SURF_SKY | SURF_NODRAW)) ||
			surface.firstVert < 0 || surface.numVerts < 3 ||
			surface.firstVert > vertCount || surface.numVerts > vertCount - surface.firstVert ||
			surface.firstIndex < 0 || surface.numIndexes < 3 ||
			surface.firstIndex > indexCount || surface.numIndexes > indexCount - surface.firstIndex ||
			surface.numIndexes % 3 != 0) {
			skipped++;
			continue;
		}
		std::unordered_map<int, uint32_t> localVertices;
		const int surfaceLightmapLayer = lightmapLumpValid &&
			(shader.surfaceFlags & SURF_NOLIGHTMAP) == 0 &&
			surface.lightmapNum >= 0 && surface.lightmapNum < g_worldLightmapLayers
			? surface.lightmapNum : -1;
		int diffuseImage = -1;
		// Only direct uncompressed/RLE TGA references are attempted. Shader scripts,
		// JPG/PNG and all stage/deform semantics intentionally use the old path.
		VRHI_LoadDiffuseTGA(shader.shader, &diffuseImage);
		const uint32_t surfaceFirstIndex = static_cast<uint32_t>(g_worldIndexes.size());
		int surfaceTriangles = 0;
		for (int i = 0; i + 2 < surface.numIndexes; i += 3) {
			const int source[3] = { VRHI_ReadIndex(indexesData, surface.firstIndex + i),
				VRHI_ReadIndex(indexesData, surface.firstIndex + i + 1),
				VRHI_ReadIndex(indexesData, surface.firstIndex + i + 2) };
			bool valid = true;
			drawVert_t vertices[3];
			glm::vec3 position[3];
			for (int corner = 0; corner < 3; ++corner) {
				if (source[corner] < 0 || source[corner] >= surface.numVerts) { valid = false; break; }
				vertices[corner] = VRHI_ReadDrawVert(vertsData,
					surface.firstVert + source[corner]);
				position[corner] = glm::vec3(vertices[corner].xyz[0], vertices[corner].xyz[1],
					vertices[corner].xyz[2]);
				if (!std::isfinite(position[corner].x) || !std::isfinite(position[corner].y) ||
					!std::isfinite(position[corner].z)) { valid = false; break; }
			}
			if (!valid || source[0] == source[1] || source[0] == source[2] || source[1] == source[2]) continue;
			const glm::vec3 edge1 = position[1] - position[0];
			const glm::vec3 edge2 = position[2] - position[0];
			const glm::vec3 cross = glm::cross(edge1, edge2);
			if (glm::dot(cross, cross) <= 1.0e-10f) continue;
			for (int corner = 0; corner < 3; ++corner) {
				std::unordered_map<int, uint32_t>::iterator found = localVertices.find(source[corner]);
				uint32_t local;
				if (found == localVertices.end()) {
					local = static_cast<uint32_t>(g_worldVertices.size());
					localVertices.emplace(source[corner], local);
					const bool validLightmapUV = surfaceLightmapLayer >= 0 &&
						std::isfinite(vertices[corner].lightmap[0]) &&
						std::isfinite(vertices[corner].lightmap[1]) &&
						vertices[corner].lightmap[0] >= 0.0f &&
						vertices[corner].lightmap[0] <= 1.0f &&
						vertices[corner].lightmap[1] >= 0.0f &&
						vertices[corner].lightmap[1] <= 1.0f;
					VRHI_WorldVertex worldVertex;
					worldVertex.position = position[corner];
					worldVertex.diffuse = (diffuseImage >= 0 &&
						std::isfinite(vertices[corner].st[0]) &&
						std::isfinite(vertices[corner].st[1]))
						? glm::vec2(vertices[corner].st[0], vertices[corner].st[1])
						: glm::vec2(0.0f);
					worldVertex.lightmap = validLightmapUV
						? glm::vec2(vertices[corner].lightmap[0], vertices[corner].lightmap[1])
						: glm::vec2(0.0f);
					worldVertex.lightmapLayer = validLightmapUV
						? static_cast<float>(surfaceLightmapLayer) : -1.0f;
					g_worldVertices.push_back(worldVertex);
					g_worldPositions.push_back(worldVertex.position);
				} else local = found->second;
				g_worldIndexes.push_back(local);
			}
			surfaceTriangles++;
		}
		if (surfaceTriangles > 0) {
			VRHI_WorldBatch batch;
			batch.firstIndex = surfaceFirstIndex;
			batch.indexCount = static_cast<uint32_t>(g_worldIndexes.size()) - surfaceFirstIndex;
			batch.diffuseImage = diffuseImage;
			g_worldBatches.push_back(batch);
			g_worldSurfaceBatch[surfaceIndex] = static_cast<int32_t>(g_worldBatches.size() - 1);
			accepted++;
		} else skipped++;
	}
	g_worldLoaded = !g_worldVertices.empty() && !g_worldIndexes.empty();
	// Per-batch visibility stamps are rebuilt whenever the batch list changes.
	g_worldBatchMarked.assign(g_worldBatches.size(), 0);
	g_worldVisEpoch = 0;
	VRHI_Printf(PRINT_ALL,
		"renderer_vrhi: BSP world '%s': models=1 surfaces=%d accepted=%d skipped=%d vertices=%zu indexes=%zu lightmaps=%d diffuse=%zu batches=%zu cull=%s planes=%d nodes=%d leafs=%d leafsurfaces=%d clusters=%d vis=%s\n",
		name, model.numSurfaces, accepted, skipped, g_worldVertices.size(), g_worldIndexes.size(),
		g_worldLightmapLayers, g_worldDiffuseImages.size(), g_worldBatches.size(),
		cullLumpsValid ? (g_worldVisAvailable ? "pvs" : "fallback-novis") : "fallback-lumps",
		planeCount, nodeCount, leafCount, leafSurfaceCount, g_worldNumClusters,
		g_worldVisAvailable ? "yes" : "no");
	g_ri.FS_FreeFile(fileData);
	if (g_worldLoaded && g_deviceInitialized) VRHI_UploadWorldGeometry();
}
static void VRHI_SetWorldVisData(const byte *vis) {
	// The engine in this tree never calls this callback; the GL renderers use
	// it to share the collision model's vis buffer. The VRHI world loader
	// instead keeps its own bounded, decoded copy of the visibility lump from
	// the BSP file, so no external pointer (and no pointer into FS data) is
	// ever retained here.
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
static glm::mat4 VRHI_QuakeViewMatrix(const refdef_t *fd) {
	glm::mat4 quakeView(1.0f);
	for (int column = 0; column < 3; ++column) {
		for (int row = 0; row < 3; ++row) quakeView[column][row] = fd->viewaxis[column][row];
		quakeView[3][column] = -fd->vieworg[0] * fd->viewaxis[column][0] -
			fd->vieworg[1] * fd->viewaxis[column][1] - fd->vieworg[2] * fd->viewaxis[column][2];
	}
	glm::mat4 flip(1.0f);
	flip[0] = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
	flip[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
	flip[2] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
	// VRHI uses column-vector matrices.  The Quake camera basis first
	// produces forward/right/up coordinates, then the coordinate conversion
	// maps them to OpenGL-style right/up/-forward clip coordinates.
	return flip * quakeView;
}

static glm::mat4 VRHI_QuakeProjection(const refdef_t *fd) {
	const float xScale = 1.0f / std::tan(fd->fov_x * 3.14159265358979323846f / 360.0f);
	const float yScale = 1.0f / std::tan(fd->fov_y * 3.14159265358979323846f / 360.0f);
	glm::mat4 projection(0.0f);
	projection[0][0] = xScale;
	// VKViewportWithDXCoords supplies a negative viewport height, so positive
	// clip-space Y maps Quake up toward the top of the framebuffer.
	projection[1][1] = yScale;
	projection[2][2] = -VRHI_WORLD_FAR / (VRHI_WORLD_FAR - VRHI_WORLD_NEAR);
	projection[3][2] = -VRHI_WORLD_FAR * VRHI_WORLD_NEAR /
		(VRHI_WORLD_FAR - VRHI_WORLD_NEAR);
	projection[2][3] = -1.0f;
	return projection;
}

// Walks the decoded BSP node tree from the root to the leaf containing point.
// Returns the leaf index, or -1 when the traversal state is malformed (out of
// range plane/node references or a child cycle). The walk is bounded by the
// number of nodes so a hostile lump can never loop forever.
static int VRHI_LocateLeaf(const glm::vec3 &point) {
	if (g_worldNodes.empty() || g_worldPlanes.empty() || g_worldLeafs.empty()) {
		return -1;
	}
	int index = 0;
	const int maxSteps = static_cast<int>(g_worldNodes.size()) + 1;
	for (int step = 0; step < maxSteps; ++step) {
		if (index < 0) {
			const int leaf = -index - 1;
			return leaf >= 0 && leaf < static_cast<int>(g_worldLeafs.size()) ? leaf : -1;
		}
		if (index >= static_cast<int>(g_worldNodes.size())) {
			return -1;
		}
		const VRHI_WorldNode &node = g_worldNodes[static_cast<size_t>(index)];
		if (node.planeNum < 0 || node.planeNum >= static_cast<int>(g_worldPlanes.size())) {
			return -1;
		}
		const VRHI_WorldPlane &plane = g_worldPlanes[static_cast<size_t>(node.planeNum)];
		const float d = glm::dot(point, plane.normal) - plane.dist;
		index = d > 0.0f ? node.children[0] : node.children[1];
	}
	return -1;
}

// Resolves the camera leaf/cluster and stamps every batch reachable through
// the visible-cluster bitset. Returns true when PVS culling was applied and
// false when the caller must fall back to drawing every batch (absent or
// malformed visibility, camera outside the tree, non-finite vieworg). The
// static vertex/index buffers are never rewritten; only the CPU-side batch
// stamps change, so the uint32 index buffer and its VRHI flags stay intact.
static bool VRHI_MarkVisibleWorldBatches(const glm::vec3 &vieworg) {
	g_worldCameraLeaf = -1;
	g_worldCameraCluster = -1;
	g_worldVisibleClusters = 0;
	g_worldVisibleBatches = 0;
	g_worldVisibleIndexes = 0;
	if (!g_worldVisAvailable || g_worldNodes.empty() || g_worldPlanes.empty() ||
		g_worldLeafs.empty() || g_worldLeafSurfaces.empty() ||
		g_worldSurfaceBatch.empty() || g_worldBatchMarked.empty() ||
		!std::isfinite(vieworg.x) || !std::isfinite(vieworg.y) || !std::isfinite(vieworg.z)) {
		return false;
	}
	const int leaf = VRHI_LocateLeaf(vieworg);
	if (leaf < 0) {
		return false;
	}
	g_worldCameraLeaf = leaf;
	const int cluster = g_worldLeafs[static_cast<size_t>(leaf)].cluster;
	if (cluster < 0 || cluster >= g_worldNumClusters) {
		return false;
	}
	g_worldCameraCluster = cluster;

	// Bump the visibility epoch, guarding against wrap-around by resetting
	// the stamps before reusing epoch 1.
	if (g_worldVisEpoch <= 0 ||
		g_worldVisEpoch == std::numeric_limits<int32_t>::max()) {
		g_worldVisEpoch = 1;
		std::fill(g_worldBatchMarked.begin(), g_worldBatchMarked.end(), 0);
	} else {
		++g_worldVisEpoch;
	}

	const byte *row = g_worldVisBits.data() +
		static_cast<size_t>(cluster) * static_cast<size_t>(g_worldClusterBytes);
	int visibleClusters = 0;
	for (int c = 0; c < g_worldNumClusters; ++c) {
		if ((row[c >> 3] & static_cast<byte>(1 << (c & 7))) != 0) {
			++visibleClusters;
		}
	}
	g_worldVisibleClusters = visibleClusters;

	uint32_t visibleIndexes = 0;
	int visibleBatches = 0;
	const int leafSurfaceCount = static_cast<int>(g_worldLeafSurfaces.size());
	for (const VRHI_WorldLeaf &leafEntry : g_worldLeafs) {
		if (leafEntry.cluster < 0 || leafEntry.cluster >= g_worldNumClusters) {
			continue;
		}
		if ((row[leafEntry.cluster >> 3] &
			static_cast<byte>(1 << (leafEntry.cluster & 7))) == 0) {
			continue;
		}
		if (leafEntry.firstLeafSurface < 0 || leafEntry.numLeafSurfaces < 0 ||
			leafEntry.firstLeafSurface > leafSurfaceCount ||
			leafEntry.numLeafSurfaces > leafSurfaceCount - leafEntry.firstLeafSurface) {
			continue;
		}
		for (int i = 0; i < leafEntry.numLeafSurfaces; ++i) {
			const int surface = g_worldLeafSurfaces[
				static_cast<size_t>(leafEntry.firstLeafSurface) + static_cast<size_t>(i)];
			if (surface < 0 || surface >= static_cast<int>(g_worldSurfaceBatch.size())) {
				continue;
			}
			const int batch = g_worldSurfaceBatch[static_cast<size_t>(surface)];
			if (batch < 0 || batch >= static_cast<int>(g_worldBatchMarked.size())) {
				continue;
			}
			if (g_worldBatchMarked[static_cast<size_t>(batch)] == g_worldVisEpoch) {
				continue;
			}
			g_worldBatchMarked[static_cast<size_t>(batch)] = g_worldVisEpoch;
			++visibleBatches;
			visibleIndexes += g_worldBatches[static_cast<size_t>(batch)].indexCount;
		}
	}
	g_worldVisibleBatches = visibleBatches;
	g_worldVisibleIndexes = visibleIndexes;
	return true;
}

// Per-frame cull diagnostics are gated by an immediate (non-latched) cheat
// cvar so hidden captures can prove cull counts without a vid_restart.
static int VRHI_CullDebugEnabled(void) {
	if (g_ri.Cvar_Get == nullptr) {
		return 0;
	}
	cvar_t *cvar = g_ri.Cvar_Get("r_vrhi_cullDebug", "0", CVAR_CHEAT);
	return cvar != nullptr ? cvar->integer : 0;
}

static void VRHI_RenderScene(const refdef_t *fd) {
	if (fd == nullptr || (fd->rdflags & (RDF_NOWORLDMODEL | RDF_HYPERSPACE)) != 0 ||
		!g_deviceInitialized || !g_frameBackbufferReady || !g_worldLoaded ||
		!g_worldShaderInitialized || g_worldVertexBuffer == VRHI_INVALID_HANDLE ||
		g_worldIndexBuffer == VRHI_INVALID_HANDLE || g_worldDepthTexture == VRHI_INVALID_HANDLE ||
		fd->width <= 0 || fd->height <= 0 || !std::isfinite(fd->fov_x) ||
		!std::isfinite(fd->fov_y) || fd->fov_x <= 0.0f || fd->fov_y <= 0.0f) return;
	if (!VRHI_CreateWorldDepth(g_frameViewportWidth, g_frameViewportHeight)) return;
	// Resolve the camera leaf/cluster and stamp the visible surface batches.
	// A failure to cull (no/malformed visibility, camera outside the tree)
	// falls back to drawing every batch, preserving the previous output.
	const glm::vec3 vieworg(fd->vieworg[0], fd->vieworg[1], fd->vieworg[2]);
	const bool cullActive = VRHI_MarkVisibleWorldBatches(vieworg);
	if (cullActive != g_worldCullActive) {
		VRHI_Printf(PRINT_ALL,
			"renderer_vrhi: world cull %s (leaf=%d cluster=%d)\n",
			cullActive ? "active" : "fallback all-visible",
			g_worldCameraLeaf, g_worldCameraCluster);
	}
	g_worldCullActive = cullActive;
	if (cullActive && !g_worldCullReported) {
		g_worldCullReported = true;
		VRHI_Printf(PRINT_ALL,
			"renderer_vrhi: PVS cull first frame: leaf=%d cluster=%d visibleClusters=%d visibleBatches=%d visibleIndexes=%u/%zu batches=%zu\n",
			g_worldCameraLeaf, g_worldCameraCluster, g_worldVisibleClusters,
			g_worldVisibleBatches, g_worldVisibleIndexes, g_worldIndexes.size(),
			g_worldBatches.size());
	}
	if (VRHI_CullDebugEnabled()) {
		VRHI_Printf(PRINT_ALL,
			"renderer_vrhi: cull frame: %s leaf=%d cluster=%d visibleClusters=%d visibleBatches=%d visibleIndexes=%u/%zu batches=%zu\n",
			cullActive ? "active" : "fallback", g_worldCameraLeaf, g_worldCameraCluster,
			g_worldVisibleClusters, g_worldVisibleBatches, g_worldVisibleIndexes,
			g_worldIndexes.size(), g_worldBatches.size());
	} else if (cullActive) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: cull frame: leaf=%d cluster=%d visibleClusters=%d visibleBatches=%d visibleIndexes=%u/%zu\n",
			g_worldCameraLeaf, g_worldCameraCluster, g_worldVisibleClusters,
			g_worldVisibleBatches, g_worldVisibleIndexes, g_worldIndexes.size());
	}
	const bool useLightmap = g_worldLightmapAvailable &&
		g_worldLightmapTexture != VRHI_INVALID_HANDLE &&
		g_worldLightmapPixelShader != VRHI_INVALID_HANDLE;
	g_worldState = g_frameState;
	g_worldState.SetColourAttachment(0, g_frameBackbuffer)
		.SetDepthAttachment(g_worldDepthTexture)
		.SetViewRect(glm::vec4(static_cast<float>(fd->x), static_cast<float>(fd->y),
			static_cast<float>(fd->width), static_cast<float>(fd->height)))
		.SetViewScissor(glm::vec4(static_cast<float>(fd->x), static_cast<float>(fd->y),
			static_cast<float>(fd->width), static_cast<float>(fd->height)))
		.SetViewClear(VRHI_CLEAR_DEPTH, glm::vec4(0.0f), 1.0f)
		.SetViewTransform(VRHI_QuakeViewMatrix(fd), VRHI_QuakeProjection(fd))
		.SetWorldTransform(glm::mat4(1.0f))
		.SetDebugFlags(VRHI_STATE_DEBUG_LOG_VATTRIB_MISMATCH |
			VRHI_STATE_DEBUG_LOG_BINDING_MISMATCH)
		.SetStateFlags(VRHI_STATE_WRITE_RGB | VRHI_STATE_WRITE_A | VRHI_STATE_WRITE_Z |
			VRHI_STATE_DEPTH_TEST_ENABLE | VRHI_STATE_DEPTH_TEST_LESS |
			VRHI_STATE_CULL_NONE | VRHI_STATE_PT_TRIANGLES)
		.SetVertexBuffer(g_worldVertexBuffer, 0, 0, 0, static_cast<uint32_t>(g_worldVertices.size()))
		.SetIndexBuffer(g_worldIndexBuffer, 0, 0, static_cast<uint32_t>(g_worldIndexes.size()))
		.SetTextures({})
		.SetSamplers({})
		.DirtyAll();
	const vhState worldBaseState = g_worldState;
	g_worldDrawErrorBaseline = g_vhErrorCounter.load(std::memory_order_relaxed);
	bool submitted = false;
	for (size_t batchIndex = 0; batchIndex < g_worldBatches.size(); ++batchIndex) {
		const VRHI_WorldBatch &batch = g_worldBatches[batchIndex];
		if (batch.indexCount == 0) continue;
		// PVS culling skips unmarked batches; the static vertex/index buffers
		// and the uint32 index buffer contents are never modified per frame.
		if (cullActive && g_worldBatchMarked[batchIndex] != g_worldVisEpoch) continue;
		const bool useDiffuse = g_worldDiffusePixelShader != VRHI_INVALID_HANDLE &&
			batch.diffuseImage >= 0 &&
			static_cast<size_t>(batch.diffuseImage) < g_worldDiffuseImages.size() &&
			g_worldDiffuseImages[batch.diffuseImage].texture != VRHI_INVALID_HANDLE;
		g_worldState = worldBaseState;
		g_worldState.SetProgram(useDiffuse ? g_worldDiffuseProgram :
			(useLightmap ? g_worldLightmapProgram : g_worldSolidProgram));
		if (useDiffuse) {
			const vhTexture diffuse = g_worldDiffuseImages[batch.diffuseImage].texture;
			g_worldState.SetTexture(0, { "u_diffuse", 0, diffuse })
				.SetSampler(0, { "u_diffuseSampler", 0,
					VRHI_SAMPLER_MIN_LINEAR | VRHI_SAMPLER_MAG_LINEAR |
					VRHI_SAMPLER_MIP_NONE | VRHI_SAMPLER_UVW_WRAP });
			if (useLightmap) {
				g_worldState.SetTexture(1, { "u_lightmap", 1, g_worldLightmapTexture })
					.SetSampler(1, { "u_lightmapSampler", 1,
						VRHI_SAMPLER_MIN_LINEAR | VRHI_SAMPLER_MAG_LINEAR |
						VRHI_SAMPLER_MIP_NONE | VRHI_SAMPLER_UVW_CLAMP });
			}
		} else if (useLightmap) {
			g_worldState.SetTexture(0, { "u_lightmap", 0, g_worldLightmapTexture })
				.SetSampler(0, { "u_lightmapSampler", 0,
					VRHI_SAMPLER_MIN_LINEAR | VRHI_SAMPLER_MAG_LINEAR |
					VRHI_SAMPLER_MIP_NONE | VRHI_SAMPLER_UVW_CLAMP });
		}
		if (vhSetState(g_worldStateId, g_worldState)) {
			if (!submitted) vhClear(g_worldStateId, VRHI_CLEAR_DEPTH);
			vhDrawIndexed(g_worldStateId, batch.indexCount, 1, batch.firstIndex);
			submitted = true;
		} else {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: world batch vhSetState failed (first=%u indexes=%u diffuse=%s lightmap=%s)\n",
				batch.firstIndex, batch.indexCount, useDiffuse ? "yes" : "no",
				useLightmap ? "yes" : "no");
		}
	}
	g_worldDrawSubmitted = submitted;
	if (!submitted) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: world draw skipped; no valid surface batches (vertices=%zu indexes=%zu)\n",
			g_worldVertices.size(), g_worldIndexes.size());
	}
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
	// Mirrors the GL renderers: without a loaded visibility set the query
	// fails (qfalse). Uses the renderer's own decoded vis copy, so it works
	// even when the engine never shares the collision model's buffer.
	if (p1 == nullptr || p2 == nullptr || !g_worldVisAvailable) {
		return qfalse;
	}
	const glm::vec3 point1(p1[0], p1[1], p1[2]);
	const glm::vec3 point2(p2[0], p2[1], p2[2]);
	const int leaf1 = VRHI_LocateLeaf(point1);
	const int leaf2 = VRHI_LocateLeaf(point2);
	if (leaf1 < 0 || leaf2 < 0) {
		return qfalse;
	}
	const int cluster1 = g_worldLeafs[static_cast<size_t>(leaf1)].cluster;
	const int cluster2 = g_worldLeafs[static_cast<size_t>(leaf2)].cluster;
	if (cluster1 < 0 || cluster1 >= g_worldNumClusters ||
		cluster2 < 0 || cluster2 >= g_worldNumClusters) {
		return qfalse;
	}
	const byte *row = g_worldVisBits.data() +
		static_cast<size_t>(cluster1) * static_cast<size_t>(g_worldClusterBytes);
	return (row[cluster2 >> 3] & static_cast<byte>(1 << (cluster2 & 7))) != 0
		? qtrue : qfalse;
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
		"renderer_vrhi: loaded (clear/present + solid-color UI + lightmapped/TGA PVS-culled BSP world)\n");
	return &exports;
}
