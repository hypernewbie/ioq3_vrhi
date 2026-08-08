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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "q_shared.h"
#include "renderercommon/tr_public.h"

// vrhi.h is the public header for the copied prebuilt VRHI library.  The
// implementation deliberately uses only its device, swapchain, state, and
// clear/present entry points for now.
#include <vrhi.h>

namespace {

static refimport_t g_ri = {};
static SDL_Window *g_window = nullptr;
static HWND g_windowHandle = nullptr;
static bool g_sdlVideoActive = false;
static bool g_sdlVideoOwned = false;
static bool g_inputInitialized = false;
static bool g_deviceInitialized = false;
static int g_windowWidth = 1280;
static int g_windowHeight = 720;
static vhState g_frameState;
static vhTexture g_frameBackbuffer = VRHI_INVALID_HANDLE;
static const vhStateId g_frameStateId = 1;

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

static void VRHI_BeginRegistration(glconfig_t *config) {
	int requestedWidth;
	int requestedHeight;
	VRHI_GetRequestedResolution(&requestedWidth, &requestedHeight);

	if (!VRHI_EnsureSDLVideo() ||
		!VRHI_CreateWindow(requestedWidth, requestedHeight) ||
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

	VRHI_FillConfig(config);
}

static void VRHI_Shutdown(qboolean destroyWindow) {
	if (!destroyWindow) {
		if (g_deviceInitialized) {
			// Keep the device and SDL window alive for a subsequent registration.
			vhFinish();
		}
		g_frameState = vhState();
		g_frameBackbuffer = VRHI_INVALID_HANDLE;
		return;
	}

	if (g_deviceInitialized) {
		// vhShutdown also waits internally, but the explicit finish makes this
		// ordering clear and guarantees the clear command queue is drained before
		// the device or native window is torn down.
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
	g_frameBackbuffer = VRHI_INVALID_HANDLE;
	g_frameState = vhState();

	if (g_sdlVideoActive || g_sdlVideoOwned) {
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}
	g_sdlVideoActive = false;
	g_sdlVideoOwned = false;
}

static void VRHI_BeginFrame(stereoFrame_t stereoFrame) {
	(void)stereoFrame;
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
	if (!vhSetState(g_frameStateId, g_frameState)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: vhSetState failed while preparing the backbuffer\n");
		return;
	}
	vhClear(g_frameStateId, VRHI_CLEAR_COLOR);
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
	// once for the engine frame. Present exactly once here.
	if (!vhFrame()) {
		const glm::uvec2 size = vhGetWindowSize();
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: vhFrame present/resize failed (%ux%u); "
			"window may be minimized or resized\n", size.x, size.y);
	}
}

// Registration, scene, image, and UI resources are intentionally not part of
// this first slice. Every callback is nevertheless populated so the client,
// cgame, and UI can safely exercise the renderer without NULL dereferences.
static qhandle_t VRHI_RegisterModel(const char *name) {
	(void)name;
	return 0;
}
static qhandle_t VRHI_RegisterSkin(const char *name) {
	(void)name;
	return 0;
}
static qhandle_t VRHI_RegisterShader(const char *name) {
	(void)name;
	return 0;
}
static qhandle_t VRHI_RegisterShaderNoMip(const char *name) {
	(void)name;
	return 0;
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
	(void)rgba;
}
static void VRHI_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader) {
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)s1;
	(void)t1;
	(void)s2;
	(void)t2;
	(void)shader;
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
		"renderer_vrhi: loaded (clear/present slice; resources/UI/world are no-op)\n");
	return &exports;
}
