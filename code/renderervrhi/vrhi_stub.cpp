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
#include <cstdint>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "q_shared.h"
#include "qcommon/qfiles.h"
#include "qcommon/surfaceflags.h"
#include "renderercommon/tr_public.h"
#include "renderervrhi/vrhi_tga_decode.h"
#include "renderervrhi/vrhi_image_decode.h"
#include "renderervrhi/vrhi_dlight.h"
#include "renderervrhi/vrhi_font.h"
#include "renderervrhi/vrhi_skin.h"
#include "renderervrhi/vrhi_beam.h"
#include "renderervrhi/vrhi_video_capture.h"
#include "renderervrhi/vrhi_shader_script.h"

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
static vhShader g_uiTexturedPixelShader = VRHI_INVALID_HANDLE;
static vhProgram g_uiProgram;
static vhProgram g_uiTexturedProgram;
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
struct VRHI_WorldVertex {
	glm::vec3 position;
	glm::vec2 diffuse;
	glm::vec2 lightmap;
	float lightmapLayer;
	glm::vec4 color;
};
// Scene submissions are retained between ClearScene and RenderScene. The
// generated geometry uses a separate bounded upload so static BSP buffers are
// never rewritten by transient entities/polys.
struct VRHI_SceneEntity {
	refEntity_t entity;
};
struct VRHI_ScenePoly {
	qhandle_t shader = 0;
	uint32_t firstVertex = 0;
	uint32_t numVerts = 0;
};
struct VRHI_SceneDraw {
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	qhandle_t shader = 0;
	// Inline BSP batches already resolved their map shader to a shared diffuse
	// image.  Keep that image index on the draw instead of manufacturing a
	// second scene-local texture table.
	int diffuseImage = -1;
	bool lightmapped = false;
};
static std::vector<VRHI_SceneEntity> g_sceneEntities;
static std::vector<VRHI_ScenePoly> g_scenePolys;
static std::vector<polyVert_t> g_scenePolyVerts;
static std::vector<VRHI_WorldVertex> g_sceneVertices;
static std::vector<uint32_t> g_sceneIndexes;
static std::vector<VRHI_SceneDraw> g_sceneDraws;
// Per-scene dynamic point lights, validated at submission and stored under
// the engine's strict MAX_DLIGHTS cap (tr_types.h). They are reset with the
// rest of the scene storage in ClearScene and on restart, and modulate the
// static world vertex color attribute plus the generated scene vertices.
static std::vector<VRHI_DLight> g_sceneLights;
static size_t g_sceneModelDraws = 0;
static vhBuffer g_sceneVertexBuffer = VRHI_INVALID_HANDLE;
static vhBuffer g_sceneIndexBuffer = VRHI_INVALID_HANDLE;
static bool g_sceneBuffersCreated = false;
static vhTexture g_worldDepthTexture = VRHI_INVALID_HANDLE;
static vhTexture g_worldLightmapTexture = VRHI_INVALID_HANDLE;
static nvrhi::Format g_worldDepthFormat = nvrhi::Format::UNKNOWN;
static std::vector<glm::vec3> g_worldPositions;
struct VRHI_WorldDiffuseImage {
	std::string path;
	int width = 0;
	int height = 0;
	bool scriptResolved = false;
	std::vector<byte> pixels;
	vhTexture texture = VRHI_INVALID_HANDLE;
};
struct VRHI_WorldBatch {
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int diffuseImage = -1;
};
struct VRHI_InlineBSPModel {
	bool valid = false;
	glm::vec3 mins = glm::vec3(0.0f);
	glm::vec3 maxs = glm::vec3(0.0f);
	std::vector<VRHI_WorldVertex> vertices;
	std::vector<uint32_t> indexes;
	std::vector<VRHI_WorldBatch> batches;
};
struct VRHI_UITexture {
	std::string path;
	int width = 0;
	int height = 0;
	std::vector<byte> pixels;
	vhTexture texture = VRHI_INVALID_HANDLE;
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
// Persistent bounded scratch for per-frame world vertex color modulation;
// sized to the world vertex count once per map and reused, never reallocated
// per frame. Only the color field changes; geometry stays untouched.
static std::vector<VRHI_WorldVertex> g_worldLightedVertices;
static std::vector<uint32_t> g_worldIndexes;
static std::vector<VRHI_WorldBatch> g_worldBatches;
static std::vector<VRHI_WorldDiffuseImage> g_worldDiffuseImages;
// Indexed by BSP model number (model 0 is the static world and is not stored
// here). Invalid entries are retained so handle refresh can never retain a
// stale index after a map reload.
static std::vector<VRHI_InlineBSPModel> g_inlineBSPModels;
static std::unordered_map<qhandle_t, size_t> g_inlineBSPModelByHandle;
// Script results are map-scoped. Values are copied qpaths, never pointers into
// FS_ListFiles/FS_ReadFile storage; an empty value is a cached miss.
static std::unordered_map<std::string, std::string> g_worldShaderScriptPaths;
static bool g_worldShaderScriptsScanned = false;
static std::vector<VRHI_UITexture> g_uiTextures;
static std::unordered_map<qhandle_t, size_t> g_uiTextureByHandle;
static std::unordered_map<qhandle_t, bool> g_uiTextureAttempts;
static std::vector<byte> g_worldLightmapPixels;

// Cinematic frames arrive as transient RGBA buffers (cols x rows x 4 bytes)
// through UploadCinematic/DrawStretchRaw. Only the GPU texture is retained
// under the strict client-slot cap and dimension/byte caps below; the caller's
// frame pointer is copied for the upload and never kept, so no unbounded frame
// data is retained. Slots are destroyed on Shutdown(qfalse) restart and
// Shutdown(qtrue) final teardown and reset on no-device failures.
static const int VRHI_MAX_CINEMATIC_CLIENTS = 8;
static const int VRHI_MAX_CINEMATIC_DIMENSION = 2048;
static const size_t VRHI_MAX_CINEMATIC_FRAME_BYTES = 16u * 1024u * 1024u;
struct VRHI_CinematicTexture {
	vhTexture texture = VRHI_INVALID_HANDLE;
	int width = 0;
	int height = 0;
	bool uploaded = false;
};
static std::vector<VRHI_CinematicTexture> g_cinematicTextures(
	VRHI_MAX_CINEMATIC_CLIENTS);

// MD3 is intentionally a data-only model slice: compressed positions/normals
// and material UVs are retained, while normal decoding/lighting and shader
// stages remain outside this renderer's scope. The limits below are independent of
// the legacy qfiles.h limits so malformed files cannot consume the renderer's
// entire image/scene budget.
static const size_t VRHI_MAX_MD3_FILE_BYTES = 64u * 1024u * 1024u;
static const size_t VRHI_MAX_MD3_MEMORY = 64u * 1024u * 1024u;
static const int VRHI_MAX_MD3_MODELS = 4096;
static const int VRHI_MAX_MD3_FRAMES = MD3_MAX_FRAMES;
static const int VRHI_MAX_MD3_SURFACES = MD3_MAX_SURFACES;
static const int VRHI_MAX_MD3_VERTICES = MD3_MAX_VERTS;
static const int VRHI_MAX_MD3_TRIANGLES = MD3_MAX_TRIANGLES;
static const int VRHI_MAX_MD3_SHADERS = MD3_MAX_SHADERS;
struct VRHI_MD3XYZ { int16_t x, y, z, normal; };
struct VRHI_MD3Tag {
	std::string name;
	glm::vec3 origin = glm::vec3(0.0f);
	glm::vec3 axis[3] = { glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f) };
};
struct VRHI_MD3Surface {
	std::string name; // lowercased surface name for customSkin overrides
	int numFrames = 0;
	int numVerts = 0;
	std::vector<glm::vec2> st;
	std::vector<VRHI_MD3XYZ> xyz;
	std::vector<uint32_t> indexes;
	qhandle_t shader = 0;
};
struct VRHI_MD3Model {
	std::string name;
	bool valid = false;
	int numFrames = 0;
	int numTags = 0;
	size_t cpuBytes = 0;
	std::vector<glm::vec3> frameMins;
	std::vector<glm::vec3> frameMaxs;
	std::vector<VRHI_MD3Tag> tags;
	std::vector<VRHI_MD3Surface> surfaces;
};
static std::vector<VRHI_MD3Model> g_md3Models;
static std::unordered_map<qhandle_t, size_t> g_md3ModelByHandle;
static size_t g_md3MemoryBytes = 0;
// Quake .skin support is a bounded surface-name -> shader-handle map parsed
// from bounded text (see vrhi_skin.h). Per-skin caps mirror the GL renderers'
// skin limits; the file/text caps and the aggregate entry cap are strict VRHI
// bounds so malformed or hostile files cannot exhaust the renderer's
// image/texture budget. The skin CPU maps are retained across Shutdown(qfalse)
// video restarts exactly like the MD3 models and are cleaned only on final
// Shutdown(qtrue) teardown.
static const int VRHI_MAX_SKINS = 1024;            // GL renderer MAX_SKINS
static const size_t VRHI_MAX_SKIN_ENTRIES_TOTAL = 8192;
struct VRHI_SkinSurface {
	std::string surface;  // lowercased MD3 surface name
	qhandle_t shader = 0; // registered shader/direct-image handle
};
struct VRHI_Skin {
	std::string name;
	std::vector<VRHI_SkinSurface> surfaces;
};
// Index 0 is unused so a skin handle maps 1:1 to a vector index; handle 0
// means "use the default skin", exactly like the GL renderers.
static std::vector<VRHI_Skin> g_skins(1);
// Lowercased name -> handle cache for already-registered skins, plus a bounded
// cache of names that failed to parse so a missing/empty skin keeps returning
// qhandle 0 without re-reading the file on every registration call.
static std::unordered_map<std::string, qhandle_t> g_skinHandles;
static std::unordered_set<std::string> g_skinFailures;
static size_t g_skinTotalEntries = 0;
static const size_t VRHI_MD3_HEADER_BYTES = 108u;
static const size_t VRHI_MD3_FRAME_BYTES = 56u;
static const size_t VRHI_MD3_TAG_BYTES = 112u;
static const size_t VRHI_MD3_SURFACE_BYTES = 108u;
static const size_t VRHI_MD3_SHADER_BYTES = 68u;
static const size_t VRHI_MD3_TRIANGLE_BYTES = 12u;
static const size_t VRHI_MD3_ST_BYTES = 8u;
static const size_t VRHI_MD3_XYZ_BYTES = 8u;
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
static void VRHI_UploadUITextures(void);
static void VRHI_DestroyCinematicTextures(void);
static bool VRHI_FiniteVec3(const float *v);
static bool VRHI_FiniteEntity(const refEntity_t &entity);
static std::string VRHI_LowerASCII(const std::string &text);
static void VRHI_DestroySceneResources(bool clearSubmissions);
static void VRHI_ResetSceneSubmissions(void);
static int32_t g_worldDrawErrorBaseline = 0;
static bool g_worldDrawSubmitted = false;
static int32_t g_uiDrawErrorBaseline = 0;
static bool g_uiDrawSubmitted = false;
static glm::vec4 g_uiColor(1.0f, 1.0f, 1.0f, 1.0f);
static int g_frameViewportWidth = 0;
static int g_frameViewportHeight = 0;
static bool g_screenshotCommandRegistered = false;
static bool g_captureRequest = false;
static std::string g_captureName;
// Only caller-owned pointers are retained for one engine frame. The request
// is consumed before present, including on failure, so stale pointers cannot
// survive a failed readback or shutdown.
static const int VRHI_MAX_VIDEO_DIMENSION = 8192;
static const size_t VRHI_MAX_VIDEO_FRAME_BYTES = 64u * 1024u * 1024u;
// AVI_LINE_PADDING from qcommon is part of the refimport ABI contract; keep
// this renderer independent of the client-only qcommon header.
static const size_t VRHI_AVI_LINE_PADDING = 4;
struct VRHI_VideoCaptureRequest {
	bool pending = false;
	int width = 0;
	int height = 0;
	byte *captureBuffer = nullptr;
	byte *encodeBuffer = nullptr;
};
static VRHI_VideoCaptureRequest g_videoCapture;
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
// Keep per-image and aggregate decoded image caps bounded when map shader
// names or image files are malformed. JPG/PNG are decoded to the same RGBA
// upload format as TGA.
static const int VRHI_MAX_WORLD_DIFFUSE_IMAGES = 256;
static const int VRHI_MAX_WORLD_DIFFUSE_DIMENSION = 2048;
static const size_t VRHI_MAX_WORLD_DIFFUSE_BYTES = 64u * 1024u * 1024u;
// Shader scripts are an intentionally small first-stage lookup, not a full
// Quake shader parser. Every list, file, aggregate text, and token stream is
// bounded before parsing or retaining a candidate path.
static const int VRHI_MAX_SHADER_FILES = 256;
static const size_t VRHI_MAX_SHADER_FILE_BYTES = 512u * 1024u;
static const size_t VRHI_MAX_SHADER_TEXT_BYTES = 8u * 1024u * 1024u;
// Token count/length bounds now live in vrhi_shader_script.h alongside the
// tokenizer that enforces them.
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
// Geometry is assembled into one static upload. Keep both aggregate and
// per-surface limits explicit so malformed BSP counts cannot grow vectors or
// temporary patch meshes without bound.
static const size_t VRHI_MAX_WORLD_VERTICES = 1024u * 1024u;
static const size_t VRHI_MAX_WORLD_INDEXES = 3u * 1024u * 1024u;
static const size_t VRHI_MAX_WORLD_BATCHES = 65536u;
static const int VRHI_MAX_WORLD_SURFACES = 65536;
static const int VRHI_MAX_WORLD_SURFACE_VERTICES = 262144;
static const int VRHI_MAX_WORLD_SURFACE_INDEXES = 786432;
// Inline BSP submodels use the same decoded surface format as the static
// world, but retain bounded local CPU geometry for per-entity transforms.
static const int VRHI_MAX_INLINE_MODELS = 1024;
static const size_t VRHI_MAX_INLINE_VERTICES = 262144u;
static const size_t VRHI_MAX_INLINE_INDEXES = 786432u;
static const size_t VRHI_MAX_INLINE_BATCHES = 16384u;
static const size_t VRHI_MAX_INLINE_SURFACE_VERTICES = 262144u;
static const size_t VRHI_MAX_INLINE_SURFACE_INDEXES = 786432u;
// Quake quadratic patches use overlapping 3x3 control-point blocks. Four
// subdivisions per block is deliberately fixed and bounded for this renderer.
static const int VRHI_PATCH_SUBDIVISIONS = 4;
static const int VRHI_MAX_PATCH_DIMENSION = 129;
static const int VRHI_MAX_PATCH_CONTROL_VERTICES = 16384;
static const int VRHI_MAX_PATCH_BLOCKS = 4096;
// UI registrations retain decoded image pixels so a video restart can destroy
// and safely re-upload GPU textures without rereading untrusted data.
static const int VRHI_MAX_UI_TEXTURES = 1024;
static const int VRHI_MAX_UI_TEXTURE_DIMENSION = 2048;
static const size_t VRHI_MAX_UI_TEXTURE_BYTES = 64u * 1024u * 1024u;
// Scene input and generated geometry are fixed-capacity per-scene storage.
// Add calls drop submissions at these limits rather than growing each frame.
static const size_t VRHI_MAX_SCENE_ENTITIES = 4096u;
// Strict per-scene dynamic light cap: the engine contract is MAX_DLIGHTS
// (32, tr_types.h) and this renderer keeps the same bound.
static const size_t VRHI_MAX_SCENE_LIGHTS = MAX_DLIGHTS;
static const size_t VRHI_MAX_SCENE_POLYS = 4096u;
static const size_t VRHI_MAX_SCENE_POLY_VERTICES = 65536u;
static const size_t VRHI_MAX_SCENE_VERTICES = 131072u;
static const size_t VRHI_MAX_SCENE_INDEXES = 393216u;
// Every generated draw (entity/quad/poly/model surface) shares one draw
// table, so the cap is the sum of the two submission sources.
static const size_t VRHI_MAX_SCENE_DRAWS = VRHI_MAX_SCENE_ENTITIES + VRHI_MAX_SCENE_POLYS;

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

struct VRHI_BackbufferReadback {
	vhMem data;
	int width = 0;
	int height = 0;
	size_t pitch = 0;
	bool inputBGRA = false;
};

static bool VRHI_ReadBackBackbuffer(vhTexture backbuffer, const char *kind,
	VRHI_BackbufferReadback *out) {
	if (out == nullptr) return false;
	*out = VRHI_BackbufferReadback();
	if (backbuffer == VRHI_INVALID_HANDLE) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: %s: invalid backbuffer handle 0x%08x\n", kind, backbuffer);
		return false;
	}
	std::vector<vhTextureMipInfo> mipInfo;
	const vhTexInfo info = vhGetTextureInfo(backbuffer, &mipInfo);
	if (info.dimensions.x <= 0 || info.dimensions.y <= 0 ||
		info.dimensions.z != 1 || info.arrayLayers != 1 ||
		info.target != nvrhi::TextureDimension::Texture2D || mipInfo.empty() ||
		info.dimensions.x > VRHI_MAX_VIDEO_DIMENSION ||
		info.dimensions.y > VRHI_MAX_VIDEO_DIMENSION) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: %s: invalid/oversized backbuffer metadata for handle "
			"0x%08x (target=%d dimensions=%dx%dx%d layers=%d mips=%zu)\n", kind,
			backbuffer, static_cast<int>(info.target), info.dimensions.x,
			info.dimensions.y, info.dimensions.z, info.arrayLayers, mipInfo.size());
		return false;
	}
	bool inputBGRA = false;
	if (!VRHI_IsScreenshotFormat(info.format, &inputBGRA)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: %s: unsupported backbuffer format %s (%d) for handle "
			"0x%08x; expected an uncompressed 4-byte RGBA/BGRA format\n", kind,
			VRHI_TextureFormatName(info.format), static_cast<int>(info.format), backbuffer);
		return false;
	}
	const vhFormatInfo formatInfo = vhGetFormat(info.format);
	if (formatInfo.elementSize != 4 || formatInfo.compressionBlockWidth > 1 ||
		formatInfo.compressionBlockHeight > 1) return false;
	const vhTextureMipInfo &baseMip = mipInfo[0];
	const size_t width = static_cast<size_t>(info.dimensions.x);
	const size_t height = static_cast<size_t>(info.dimensions.y);
	const size_t rowBytes = width * 4;
	if (height > (std::numeric_limits<size_t>::max)() / rowBytes ||
		rowBytes * height > VRHI_MAX_VIDEO_FRAME_BYTES || baseMip.pitch <= 0 ||
		static_cast<size_t>(baseMip.pitch) < rowBytes || baseMip.slice_size <= 0 ||
		static_cast<uint64_t>(baseMip.slice_size) > VRHI_MAX_VIDEO_FRAME_BYTES ||
		static_cast<uint64_t>(baseMip.slice_size) <
			static_cast<uint64_t>(baseMip.pitch) * height) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: %s: invalid readback layout for handle 0x%08x\n",
			kind, backbuffer);
		return false;
	}
	const size_t expectedBytes = static_cast<size_t>(baseMip.slice_size);
	const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
	vhReadTextureSlow(backbuffer, 0, 0, &out->data);
	vhFinish();
	const int32_t errorsAfter = g_vhErrorCounter.load(std::memory_order_relaxed);
	if (errorsAfter != errorsBefore || out->data.size() != expectedBytes) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: %s: readback failed for handle 0x%08x; got %zu, expected %zu\n",
			kind, backbuffer, out->data.size(), expectedBytes);
		return false;
	}
	out->width = info.dimensions.x;
	out->height = info.dimensions.y;
	out->pitch = static_cast<size_t>(baseMip.pitch);
	out->inputBGRA = inputBGRA;
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

static void VRHI_CaptureVideoFrame(void) {
	const VRHI_VideoCaptureRequest request = g_videoCapture;
	g_videoCapture = VRHI_VideoCaptureRequest();
	if (!request.pending || !g_frameBackbufferReady || request.encodeBuffer == nullptr) {
		if (request.pending) VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: AVI video capture dropped: no usable backbuffer or encode buffer\n");
		return;
	}
	VRHI_BackbufferReadback readback;
	if (!VRHI_ReadBackBackbuffer(g_frameBackbuffer, "AVI video capture", &readback) ||
		readback.width != request.width || readback.height != request.height) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: AVI video capture dropped: backbuffer dimensions do not match %dx%d\n",
			request.width, request.height);
		return;
	}
	const size_t width = static_cast<size_t>(request.width);
	const size_t height = static_cast<size_t>(request.height);
	const size_t aviLineBytes = width * 3;
	const size_t aviPitch = (aviLineBytes + (VRHI_AVI_LINE_PADDING - 1)) /
		VRHI_AVI_LINE_PADDING * VRHI_AVI_LINE_PADDING;
	if (height > (std::numeric_limits<size_t>::max)() / aviPitch ||
		aviPitch * height > VRHI_MAX_VIDEO_FRAME_BYTES ||
		aviPitch * height > static_cast<size_t>(std::numeric_limits<int>::max()) ||
		!vrhi_video::ConvertTopFirstRGBA8ToBottomUpBGR24(
			readback.data.data(), readback.pitch, request.width, request.height,
			readback.inputBGRA, request.encodeBuffer, aviPitch, aviPitch * height)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: AVI video capture dropped: bounded pixel conversion failed\n");
		return;
	}
	if (request.captureBuffer != nullptr) {
		const size_t capturePitch = width * 4;
		for (size_t y = 0; y < height; ++y) {
			std::memcpy(request.captureBuffer + y * capturePitch,
				readback.data.data() + y * readback.pitch, capturePitch);
		}
	}
	if (g_ri.CL_WriteAVIVideoFrame != nullptr) {
		g_ri.CL_WriteAVIVideoFrame(request.encodeBuffer, static_cast<int>(aviPitch * height));
	} else {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: AVI video capture dropped: writer is unavailable\n");
	}
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
    float4 color : TEXCOORD3;
};
[shader("vertex")]
VSOutput main(float3 position : POSITION, float2 diffuse : TEXCOORD0,
    float2 lightmap : TEXCOORD1, float lightmapLayer : TEXCOORD2,
    float4 color : TEXCOORD3)
{
    VSOutput output;
    output.position = mul(u_worldViewProj, float4(position, 1.0));
    output.diffuse = diffuse;
    output.lightmap = lightmap;
    output.lightmapLayer = lightmapLayer;
    output.color = color;
    return output;
}
)";

static const char *VRHI_WorldSolidPixelSource = R"(
struct PSInput {
    float4 position : SV_Position;
    float2 diffuse : TEXCOORD0;
    float2 lightmap : TEXCOORD1;
    float lightmapLayer : TEXCOORD2;
    float4 color : TEXCOORD3;
};
[shader("pixel")]
float4 main(PSInput input) : SV_Target
{
    return float4(0.24, 0.42, 0.22, 1.0) * input.color;
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
    float4 color : TEXCOORD3;
};
[shader("pixel")]
float4 main(PSInput input) : SV_Target
{
    const float4 solid = float4(0.24, 0.42, 0.22, 1.0);
    if (input.lightmapLayer < -0.5)
        return solid * input.color;
    return float4(u_lightmap.Sample(u_lightmapSampler,
        float3(saturate(input.lightmap), input.lightmapLayer)).rgb, 1.0) * input.color;
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
    float4 color : TEXCOORD3;
};
[shader("pixel")]
float4 main(PSInput input) : SV_Target
{
    float3 color = u_diffuse.Sample(u_diffuseSampler, input.diffuse).rgb;
    if (input.lightmapLayer >= -0.5)
        color *= u_lightmap.Sample(u_lightmapSampler,
            float3(saturate(input.lightmap), input.lightmapLayer)).rgb;
    return float4(color, 1.0) * input.color;
}
)";

// UI uses a procedural six-vertex rectangle. The solid program remains the
// explicit fallback for unsupported handles; the textured program shares the
// vertex shader so DrawStretchPic's UV rectangle is preserved exactly.
static const char *VRHI_UIVertexSource = R"(
cbuffer globalParams : register(b300, VRHI_STAGE_SPACE)
{
    float4 ui_rect;
    float4 ui_uv;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
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
    output.texcoord = ui_uv.xy + corners[vertexID] * (ui_uv.zw - ui_uv.xy);
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

static const char *VRHI_UITexturedPixelSource = R"(
Texture2D<float4> ui_texture : register(t0, VRHI_STAGE_SPACE);
SamplerState ui_sampler : register(s0, VRHI_STAGE_SPACE);
cbuffer globalParams : register(b300, VRHI_STAGE_SPACE)
{
    float4 ui_color;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

[shader("pixel")]
float4 main(PSInput input) : SV_Target
{
    return ui_texture.Sample(ui_sampler, input.texcoord) * ui_color;
}
)";

static void VRHI_DestroyWorldResources(bool clearGeometry) {
	VRHI_DestroySceneResources(clearGeometry);
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
		g_worldLightedVertices.clear();
		g_worldIndexes.clear();
		g_worldBatches.clear();
		g_worldDiffuseImages.clear();
		g_inlineBSPModels.clear();
		g_inlineBSPModelByHandle.clear();
		g_worldShaderScriptPaths.clear();
		g_worldShaderScriptsScanned = false;
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
			VRHI_Printf(PRINT_WARNING, "renderer_vrhi: diffuse image '%s' allocation failed; using lightmap/solid fallback\n", image.path.c_str());
			continue;
		}
		vhMem *data = new vhMem(image.pixels.size());
		std::memcpy(data->data(), image.pixels.data(), data->size());
		const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
		vhCreateTexture2D(texture, image.path.c_str(), glm::ivec2(image.width, image.height), 1,
			nvrhi::Format::RGBA8_UNORM, VRHI_TEXTURE_NONE | VRHI_SAMPLER_NONE, data);
		vhFinish();
		if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
			VRHI_Printf(PRINT_WARNING, "renderer_vrhi: diffuse image '%s' upload failed; using lightmap/solid fallback\n", image.path.c_str());
			vhDestroyTexture(texture);
			vhFinish();
			continue;
		}
		image.texture = texture;
		anyUploaded = true;
		VRHI_Printf(PRINT_ALL, "renderer_vrhi: uploaded %sBSP diffuse '%s' (%dx%d, %zu bytes)\n",
			image.scriptResolved ? "script-resolved " : "", image.path.c_str(),
			image.width, image.height, image.pixels.size());
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
		"float3 float2 float2 float float4", g_worldVertices.size());
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

static void VRHI_DestroySceneResources(bool clearSubmissions) {
	if (g_deviceInitialized) vhFinish();
	if (g_sceneVertexBuffer != VRHI_INVALID_HANDLE) {
		vhDestroyBuffer(g_sceneVertexBuffer);
		g_sceneVertexBuffer = VRHI_INVALID_HANDLE;
	}
	if (g_sceneIndexBuffer != VRHI_INVALID_HANDLE) {
		vhDestroyBuffer(g_sceneIndexBuffer);
		g_sceneIndexBuffer = VRHI_INVALID_HANDLE;
	}
	g_sceneBuffersCreated = false;
	if (g_deviceInitialized) vhFinish();
	if (clearSubmissions) VRHI_ResetSceneSubmissions();
}

static void VRHI_ResetSceneSubmissions(void) {
	g_sceneEntities.clear();
	g_scenePolys.clear();
	g_scenePolyVerts.clear();
	g_sceneVertices.clear();
	g_sceneIndexes.clear();
	g_sceneDraws.clear();
	g_sceneLights.clear();
	g_sceneModelDraws = 0;
}

static void VRHI_ReserveSceneStorage(void) {
	// reserve is performed once before the first scene and all Add calls are
	// capped, so a hostile cgame cannot cause unbounded per-frame allocation.
	if (g_sceneEntities.capacity() < VRHI_MAX_SCENE_ENTITIES)
		g_sceneEntities.reserve(VRHI_MAX_SCENE_ENTITIES);
	if (g_scenePolys.capacity() < VRHI_MAX_SCENE_POLYS)
		g_scenePolys.reserve(VRHI_MAX_SCENE_POLYS);
	if (g_scenePolyVerts.capacity() < VRHI_MAX_SCENE_POLY_VERTICES)
		g_scenePolyVerts.reserve(VRHI_MAX_SCENE_POLY_VERTICES);
	if (g_sceneVertices.capacity() < VRHI_MAX_SCENE_VERTICES)
		g_sceneVertices.reserve(VRHI_MAX_SCENE_VERTICES);
	if (g_sceneIndexes.capacity() < VRHI_MAX_SCENE_INDEXES)
		g_sceneIndexes.reserve(VRHI_MAX_SCENE_INDEXES);
	if (g_sceneDraws.capacity() < VRHI_MAX_SCENE_DRAWS)
		g_sceneDraws.reserve(VRHI_MAX_SCENE_DRAWS);
	if (g_sceneLights.capacity() < VRHI_MAX_SCENE_LIGHTS)
		g_sceneLights.reserve(VRHI_MAX_SCENE_LIGHTS);
}

static void VRHI_DestroyUITextures(bool clearData) {
	if (g_deviceInitialized) {
		vhFinish();
	}
	for (VRHI_UITexture &image : g_uiTextures) {
		if (image.texture != VRHI_INVALID_HANDLE) {
			vhDestroyTexture(image.texture);
			image.texture = VRHI_INVALID_HANDLE;
		}
	}
	if (g_deviceInitialized) {
		vhFinish();
	}
	if (clearData) {
		g_uiTextures.clear();
		g_uiTextureByHandle.clear();
		g_uiTextureAttempts.clear();
	}
}

static void VRHI_DestroyCinematicTextures(void) {
	// Cinematic slots retain only GPU textures (never frame data), so this is
	// a full reset on both restart and final shutdown. Slots stay allocated so
	// out-of-range client indices remain a strict no-op.
	if (g_deviceInitialized) {
		vhFinish();
	}
	for (VRHI_CinematicTexture &slot : g_cinematicTextures) {
		if (slot.texture != VRHI_INVALID_HANDLE) {
			vhDestroyTexture(slot.texture);
			slot.texture = VRHI_INVALID_HANDLE;
		}
		slot.width = 0;
		slot.height = 0;
		slot.uploaded = false;
	}
	if (g_deviceInitialized) {
		vhFinish();
	}
}

static void VRHI_DestroyMD3Resources(bool clearData) {
	if (!clearData) return;
	g_md3Models.clear();
	g_md3ModelByHandle.clear();
	g_md3MemoryBytes = 0;
}

static void VRHI_DestroySkinResources(bool clearData) {
	if (!clearData) return;
	g_skins.clear();
	g_skins.emplace_back(); // keep index 0 reserved (handle 0 = default skin)
	g_skinHandles.clear();
	g_skinFailures.clear();
	g_skinTotalEntries = 0;
}

static void VRHI_DestroyUI(void) {
	VRHI_DestroyUITextures(true);
	if (g_uiVertexShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_uiVertexShader);
	}
	if (g_uiPixelShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_uiPixelShader);
	}
	if (g_uiTexturedPixelShader != VRHI_INVALID_HANDLE) {
		vhDestroyShader(g_uiTexturedPixelShader);
	}
	g_uiVertexShader = VRHI_INVALID_HANDLE;
	g_uiPixelShader = VRHI_INVALID_HANDLE;
	g_uiTexturedPixelShader = VRHI_INVALID_HANDLE;
	g_uiProgram.clear();
	g_uiTexturedProgram.clear();
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
	std::vector<uint32_t> texturedPixelSpirv;
	std::string error;
	if (!vhCompileShader("VRHI_UIVertex", VRHI_UIVertexSource,
		VRHI_SHADER_STAGE_VERTEX | VRHI_SHADER_SM_6_0, vertexSpirv, "main",
		{}, {}, &error)) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: UI vertex shader compile failed: %s\n", error.c_str());
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
	std::string texturedError;
	const bool texturedCompiled = vhCompileShader("VRHI_UITexturedPixel",
		VRHI_UITexturedPixelSource,
		VRHI_SHADER_STAGE_PIXEL | VRHI_SHADER_SM_6_0, texturedPixelSpirv, "main",
		{}, {}, &texturedError);
	if (!texturedCompiled) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: textured UI pixel shader unavailable; direct images use solid fallback: %s\n",
			texturedError.c_str());
	}

	g_uiVertexShader = vhAllocShader();
	g_uiPixelShader = vhAllocShader();
	if (texturedCompiled) g_uiTexturedPixelShader = vhAllocShader();
	if (g_uiVertexShader == VRHI_INVALID_HANDLE ||
		g_uiPixelShader == VRHI_INVALID_HANDLE ||
		(texturedCompiled && g_uiTexturedPixelShader == VRHI_INVALID_HANDLE)) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: UI shader allocation failed\n");
		VRHI_DestroyUI();
		return false;
	}

	const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
	vhCreateShader(g_uiVertexShader, "VRHI_UIVertex", VRHI_SHADER_STAGE_VERTEX,
		vertexSpirv, "main");
	vhCreateShader(g_uiPixelShader, "VRHI_UIPixel", VRHI_SHADER_STAGE_PIXEL,
		pixelSpirv, "main");
	if (texturedCompiled) {
		vhCreateShader(g_uiTexturedPixelShader, "VRHI_UITexturedPixel",
			VRHI_SHADER_STAGE_PIXEL, texturedPixelSpirv, "main");
	}
	vhFinish();
	const int32_t errorsAfter = g_vhErrorCounter.load(std::memory_order_relaxed);
	if (errorsAfter != errorsBefore) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: UI shader creation failed (VRHI errors %+d)\n",
			static_cast<int>(errorsAfter - errorsBefore));
		VRHI_DestroyUI();
		return false;
	}

	g_uiProgram = vhCreateGfxProgram(g_uiVertexShader, g_uiPixelShader);
	if (texturedCompiled) {
		g_uiTexturedProgram = vhCreateGfxProgram(g_uiVertexShader,
			g_uiTexturedPixelShader);
	}
	g_uiInitialized = true;
	VRHI_Printf(PRINT_ALL,
		"renderer_vrhi: UI ready (direct image textures=%s, solid fallback=yes)\n",
		!g_uiTexturedProgram.empty() ? "yes" : "no");
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

	// Compile UI programs only after vhInit has created a device; a shader
	// failure leaves clear/present and solid-color fallback callbacks usable.
	VRHI_InitializeUI();
	VRHI_UploadUITextures();
	VRHI_InitializeWorldShader();
	if (g_worldLoaded) VRHI_UploadWorldGeometry();
	VRHI_CreateWorldDepth(g_windowWidth, g_windowHeight);
	VRHI_FillConfig(config);
}

static void VRHI_Shutdown(qboolean destroyWindow) {
	if (!destroyWindow) {
		// Keep the device and SDL window alive for a subsequent registration,
		// but release map/video resources before the swapchain is reused. The
		// unconditional CPU cleanup also clears script caches if registration
		// failed before a device was created.
		VRHI_DestroyWorldResources(true);
		g_videoCapture = VRHI_VideoCaptureRequest();
		if (g_deviceInitialized) {
			VRHI_DestroyUITextures(false);
			VRHI_DestroyCinematicTextures();
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
	g_videoCapture = VRHI_VideoCaptureRequest();
	VRHI_RemoveScreenshotCommand();

	if (g_deviceInitialized) {
		// vhShutdown also waits internally, but the explicit finish makes this
		// ordering clear and guarantees the clear command queue is drained before
		// the device or native window is torn down.
		vhFinish();
	}
	// Also clear decoded world/script and UI data when registration failed
	// before a device was created; final shutdown must not rely on DLL unload
	// for CPU ownership.
	VRHI_DestroyWorldResources(true);
	VRHI_DestroyUI();
	VRHI_DestroyMD3Resources(true);
	VRHI_DestroySkinResources(true);
	VRHI_DestroyCinematicTextures();
	if (g_deviceInitialized) {
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
	// once for the engine frame. Present exactly once here. Screenshot and AVI
	// capture happen synchronously before this present.
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
	if (g_videoCapture.pending) {
		VRHI_CaptureVideoFrame();
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
	if (g_uiDrawSubmitted) {
		const int32_t errors = g_vhErrorCounter.load(std::memory_order_relaxed) -
			g_uiDrawErrorBaseline;
		if (errors > 0) {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: UI draw reported %d VRHI error(s)\n", errors);
		}
		g_uiDrawSubmitted = false;
	}
	// Never let a later EndFrame reuse a framebuffer from after present.
	g_frameBackbufferReady = false;
	g_frameBackbuffer = VRHI_INVALID_HANDLE;
}

// RT_SPRITE and the RT_BEAM/RT_LIGHTNING/RT_RAIL_CORE/RT_RAIL_RINGS
// beam-quad fallback plus AddPolyToScene are retained in bounded CPU scene
// storage and uploaded after the static BSP world. DrawStretchPic supports
// bounded direct image UI textures and retains a solid-color fallback for
// missing/unsupported handles, while the first BSP model and its bounded image
// diffuse batches are rendered by the static world path above. RT_MODEL covers
// bounded MD3 and inline BSP submodels: MD3 surfaces select entity.customShader
// first, then the customSkin surface-name override, then the embedded surface
// shader. Complex effect/material stages remain explicit safe no-ops. Every
// callback is nevertheless populated so the client, cgame, and UI can safely
// exercise the renderer without NULL dereferences.
//
// The four registration callbacks return stable engine-local qhandles so the
// client, cgame, and UI see successful registrations (qhandle_t 0 means
// failure). The handle policy mirrors the GL renderers: each handle space is
// independent, the first handle is 1, the same name always resolves to the
// same handle, and NULL/empty names fail with 0. RegisterSkin parses bounded
// Quake .skin text (line/comment/whitespace handling, surface,shader entries,
// safe qpaths, and file/text/entry/path caps) into surface-name shader
// overrides that reuse the shared bounded direct-image handle path; unsupported
// shader scripts/materials keep the safe solid fallback, skinNum remains
// unsupported, and general shader-script/PK3 material semantics stay out of
// scope; bounded direct image data is the sole uploaded UI material.
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
static std::unordered_map<std::string, qhandle_t> g_shaderHandles;

static bool VRHI_RegisterDirectUITexture(qhandle_t handle, const char *name);
static void VRHI_UploadUITextures(void);

static bool VRHI_MD3Range(size_t offset, size_t bytes, size_t limit) {
	return offset <= limit && bytes <= limit - offset;
}
static int32_t VRHI_MD3Int(const byte *p) {
	int32_t value;
	std::memcpy(&value, p, sizeof(value));
	return LittleLong(value);
}
static float VRHI_MD3Float(const byte *p) {
	float value;
	std::memcpy(&value, p, sizeof(value));
	return LittleFloat(value);
}
static int16_t VRHI_MD3Short(const byte *p) {
	int16_t value;
	std::memcpy(&value, p, sizeof(value));
	return static_cast<int16_t>(LittleShort(value));
}
static bool VRHI_MD3String(const byte *p, size_t maxBytes, std::string *out) {
	size_t length = 0;
	while (length < maxBytes && p[length] != '\0') ++length;
	if (length == maxBytes) return false;
	if (out != nullptr) out->assign(reinterpret_cast<const char *>(p), length);
	return true;
}
static bool VRHI_MD3SafeName(const char *name, size_t *lengthOut) {
	if (name == nullptr || name[0] == '\0') return false;
	size_t length = 0;
	while (length < MAX_QPATH && name[length] != '\0') ++length;
	if (length == 0 || length >= MAX_QPATH || name[0] == '*' ||
		std::strstr(name, "..") != nullptr) return false;
	if (lengthOut != nullptr) *lengthOut = length;
	return true;
}
static bool VRHI_MD3Extension(const char *name) {
	if (name == nullptr) return false;
	const char *slash = std::strrchr(name, '/');
	const char *backslash = std::strrchr(name, '\\'); // a single '\' escape: the Windows path separator
	const char *base = slash != nullptr && slash > backslash ? slash + 1 :
		(backslash != nullptr ? backslash + 1 : name);
	const char *dot = std::strrchr(base, '.');
	if (dot == nullptr || dot[1] == '\0') return false;
	char ext[5] = {};
	size_t length = std::strlen(dot + 1);
	if (length != 3) return false;
	for (size_t i = 0; i < length; ++i)
		ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(dot[1 + i])));
	return std::strcmp(ext, "md3") == 0;
}
static bool VRHI_InlineModelName(const char *name, int *modelIndex) {
	if (modelIndex != nullptr) *modelIndex = -1;
	if (name == nullptr || name[0] != '*' || name[1] == '0' ||
		name[1] < '1' || name[1] > '9') return false;
	int value = 0;
	for (size_t i = 1; name[i] != '\0'; ++i) {
		if (i >= 5 || name[i] < '0' || name[i] > '9' ||
		value > (VRHI_MAX_INLINE_MODELS - (name[i] - '0')) / 10) return false;
		value = value * 10 + (name[i] - '0');
	}
	if (value < 1 || value > VRHI_MAX_INLINE_MODELS) return false;
	if (modelIndex != nullptr) *modelIndex = value;
	return true;
}

static void VRHI_RefreshInlineModelHandles(void) {
	g_inlineBSPModelByHandle.clear();
	for (const auto &entry : g_modelHandles) {
		int modelIndex = -1;
		if (!VRHI_InlineModelName(entry.first.c_str(), &modelIndex) ||
			static_cast<size_t>(modelIndex) >= g_inlineBSPModels.size() ||
			!g_inlineBSPModels[static_cast<size_t>(modelIndex)].valid) continue;
		g_inlineBSPModelByHandle.emplace(entry.second, static_cast<size_t>(modelIndex));
		VRHI_Printf(PRINT_ALL, "renderer_vrhi: registered inline BSP '%s' handle=%d model=%d\n",
			entry.first.c_str(), static_cast<int>(entry.second), modelIndex);
	}
}

static qhandle_t VRHI_RegisterModelShader(const std::string &shaderName) {
	if (shaderName.empty()) return 0;
	const qhandle_t handle = VRHI_RegisterName(g_shaderHandles, shaderName.c_str(),
		"RegisterModelShader");
	if (handle != 0) VRHI_RegisterDirectUITexture(handle, shaderName.c_str());
	return handle;
}

static bool VRHI_ParseMD3(const byte *file, size_t fileSize, const char *name,
	VRHI_MD3Model *out) {
	if (file == nullptr || out == nullptr || fileSize < VRHI_MD3_HEADER_BYTES ||
		fileSize > VRHI_MAX_MD3_FILE_BYTES) return false;
	const int32_t ident = VRHI_MD3Int(file + 0);
	const int32_t version = VRHI_MD3Int(file + 4);
	if (ident != MD3_IDENT || version != MD3_VERSION) return false;
	const int32_t numFrames = VRHI_MD3Int(file + 76);
	const int32_t numTags = VRHI_MD3Int(file + 80);
	const int32_t numSurfaces = VRHI_MD3Int(file + 84);
	const int32_t ofsFrames = VRHI_MD3Int(file + 92);
	const int32_t ofsTags = VRHI_MD3Int(file + 96);
	const int32_t ofsSurfaces = VRHI_MD3Int(file + 100);
	const int32_t ofsEnd = VRHI_MD3Int(file + 104);
	if (numFrames <= 0 || numFrames > VRHI_MAX_MD3_FRAMES || numTags < 0 ||
		numTags > MD3_MAX_TAGS || numSurfaces < 0 ||
		numSurfaces > VRHI_MAX_MD3_SURFACES || ofsEnd < static_cast<int32_t>(VRHI_MD3_HEADER_BYTES) ||
		static_cast<size_t>(ofsEnd) > fileSize) return false;
	const size_t end = static_cast<size_t>(ofsEnd);
	if (static_cast<size_t>(numFrames) > std::numeric_limits<size_t>::max() / VRHI_MD3_FRAME_BYTES ||
		!VRHI_MD3Range(static_cast<size_t>(ofsFrames), static_cast<size_t>(numFrames) * VRHI_MD3_FRAME_BYTES, end) ||
		static_cast<size_t>(numTags) > std::numeric_limits<size_t>::max() / VRHI_MD3_TAG_BYTES ||
		!VRHI_MD3Range(static_cast<size_t>(ofsTags), static_cast<size_t>(numFrames) * static_cast<size_t>(numTags) * VRHI_MD3_TAG_BYTES, end) ||
		(numSurfaces > 0 && !VRHI_MD3Range(static_cast<size_t>(ofsSurfaces), VRHI_MD3_SURFACE_BYTES, end))) return false;
	try {
		VRHI_MD3Model model;
		model.name = name;
		model.numFrames = numFrames;
		model.numTags = numTags;
		model.frameMins.reserve(static_cast<size_t>(numFrames));
		model.frameMaxs.reserve(static_cast<size_t>(numFrames));
		for (int frame = 0; frame < numFrames; ++frame) {
			const byte *p = file + static_cast<size_t>(ofsFrames) + static_cast<size_t>(frame) * VRHI_MD3_FRAME_BYTES;
			glm::vec3 mins(VRHI_MD3Float(p + 0), VRHI_MD3Float(p + 4), VRHI_MD3Float(p + 8));
			glm::vec3 maxs(VRHI_MD3Float(p + 12), VRHI_MD3Float(p + 16), VRHI_MD3Float(p + 20));
			const glm::vec3 localOrigin(VRHI_MD3Float(p + 24), VRHI_MD3Float(p + 28), VRHI_MD3Float(p + 32));
			const float radius = VRHI_MD3Float(p + 36);
			std::string frameName;
			if (!VRHI_MD3String(p + 40, 16, &frameName) ||
				!std::isfinite(mins.x) || !std::isfinite(mins.y) || !std::isfinite(mins.z) ||
				!std::isfinite(maxs.x) || !std::isfinite(maxs.y) || !std::isfinite(maxs.z) ||
				!std::isfinite(localOrigin.x) || !std::isfinite(localOrigin.y) || !std::isfinite(localOrigin.z) ||
				!std::isfinite(radius)) return false;
			model.frameMins.push_back(mins);
			model.frameMaxs.push_back(maxs);
		}
		const size_t tagCount = static_cast<size_t>(numFrames) * static_cast<size_t>(numTags);
		model.tags.reserve(tagCount);
		for (size_t i = 0; i < tagCount; ++i) {
			const byte *p = file + static_cast<size_t>(ofsTags) + i * VRHI_MD3_TAG_BYTES;
			VRHI_MD3Tag tag;
			if (!VRHI_MD3String(p, MAX_QPATH, &tag.name)) return false;
			tag.origin = glm::vec3(VRHI_MD3Float(p + 64), VRHI_MD3Float(p + 68), VRHI_MD3Float(p + 72));
			for (int axis = 0; axis < 3; ++axis) tag.axis[axis] = glm::vec3(
				VRHI_MD3Float(p + 76 + axis * 12), VRHI_MD3Float(p + 80 + axis * 12), VRHI_MD3Float(p + 84 + axis * 12));
			if (!std::isfinite(tag.origin.x) || !std::isfinite(tag.origin.y) || !std::isfinite(tag.origin.z)) return false;
			for (int axis = 0; axis < 3; ++axis) if (!std::isfinite(tag.axis[axis].x) || !std::isfinite(tag.axis[axis].y) || !std::isfinite(tag.axis[axis].z)) return false;
			model.tags.push_back(std::move(tag));
		}
		size_t surfaceOffset = static_cast<size_t>(ofsSurfaces);
		for (int surfaceIndex = 0; surfaceIndex < numSurfaces; ++surfaceIndex) {
			if (!VRHI_MD3Range(surfaceOffset, VRHI_MD3_SURFACE_BYTES, end)) return false;
			const byte *p = file + surfaceOffset;
			if (VRHI_MD3Int(p + 0) != MD3_IDENT) return false;
			const int32_t surfaceFrames = VRHI_MD3Int(p + 72);
			const int32_t shaderCount = VRHI_MD3Int(p + 76);
			const int32_t numVerts = VRHI_MD3Int(p + 80);
			const int32_t numTriangles = VRHI_MD3Int(p + 84);
			const int32_t ofsTriangles = VRHI_MD3Int(p + 88);
			const int32_t ofsShaders = VRHI_MD3Int(p + 92);
			const int32_t ofsSt = VRHI_MD3Int(p + 96);
			const int32_t ofsXYZ = VRHI_MD3Int(p + 100);
			const int32_t surfaceEndDisk = VRHI_MD3Int(p + 104);
			if (surfaceFrames != numFrames || shaderCount < 0 || shaderCount > VRHI_MAX_MD3_SHADERS ||
				numVerts <= 0 || numVerts > VRHI_MAX_MD3_VERTICES || numTriangles <= 0 ||
				numTriangles > VRHI_MAX_MD3_TRIANGLES || surfaceEndDisk < static_cast<int32_t>(VRHI_MD3_SURFACE_BYTES) ||
				static_cast<size_t>(surfaceEndDisk) > end || !VRHI_MD3Range(surfaceOffset, static_cast<size_t>(surfaceEndDisk), end)) return false;
			const size_t surfaceEnd = surfaceOffset + static_cast<size_t>(surfaceEndDisk);
			if (ofsTriangles < static_cast<int32_t>(VRHI_MD3_SURFACE_BYTES) ||
				ofsShaders < static_cast<int32_t>(VRHI_MD3_SURFACE_BYTES) ||
				ofsSt < static_cast<int32_t>(VRHI_MD3_SURFACE_BYTES) ||
				ofsXYZ < static_cast<int32_t>(VRHI_MD3_SURFACE_BYTES) ||
				static_cast<size_t>(ofsTriangles) > static_cast<size_t>(surfaceEndDisk) ||
				static_cast<size_t>(ofsShaders) > static_cast<size_t>(surfaceEndDisk) ||
				static_cast<size_t>(ofsSt) > static_cast<size_t>(surfaceEndDisk) ||
				static_cast<size_t>(ofsXYZ) > static_cast<size_t>(surfaceEndDisk) ||
				static_cast<size_t>(numTriangles) > std::numeric_limits<size_t>::max() / VRHI_MD3_TRIANGLE_BYTES ||
				static_cast<size_t>(shaderCount) > std::numeric_limits<size_t>::max() / VRHI_MD3_SHADER_BYTES ||
				static_cast<size_t>(numVerts) > std::numeric_limits<size_t>::max() / VRHI_MD3_ST_BYTES ||
				static_cast<size_t>(numFrames) * static_cast<size_t>(numVerts) > std::numeric_limits<size_t>::max() / VRHI_MD3_XYZ_BYTES ||
				!VRHI_MD3Range(surfaceOffset + static_cast<size_t>(ofsTriangles), static_cast<size_t>(numTriangles) * VRHI_MD3_TRIANGLE_BYTES, surfaceEnd) ||
				!VRHI_MD3Range(surfaceOffset + static_cast<size_t>(ofsShaders), static_cast<size_t>(shaderCount) * VRHI_MD3_SHADER_BYTES, surfaceEnd) ||
				!VRHI_MD3Range(surfaceOffset + static_cast<size_t>(ofsSt), static_cast<size_t>(numVerts) * VRHI_MD3_ST_BYTES, surfaceEnd) ||
				!VRHI_MD3Range(surfaceOffset + static_cast<size_t>(ofsXYZ), static_cast<size_t>(numFrames) * static_cast<size_t>(numVerts) * VRHI_MD3_XYZ_BYTES, surfaceEnd)) return false;
			VRHI_MD3Surface surface;
			// Retain the surface name (lowercased, like the GL renderers) so a
			// customSkin can override this surface by name during RT_MODEL draws.
			if (!VRHI_MD3String(p + 4, MAX_QPATH, &surface.name)) return false;
			surface.name = VRHI_LowerASCII(surface.name);
			surface.numFrames = numFrames;
			surface.numVerts = numVerts;
			surface.st.reserve(static_cast<size_t>(numVerts));
			surface.xyz.reserve(static_cast<size_t>(numFrames) * static_cast<size_t>(numVerts));
			surface.indexes.reserve(static_cast<size_t>(numTriangles) * 3u);
			for (int shader = 0; shader < shaderCount; ++shader) {
				std::string shaderName;
				if (!VRHI_MD3String(file + surfaceOffset + static_cast<size_t>(ofsShaders) + static_cast<size_t>(shader) * VRHI_MD3_SHADER_BYTES, MAX_QPATH, &shaderName)) return false;
				if (shader == 0) surface.shader = VRHI_RegisterModelShader(shaderName);
			}
			for (int vertex = 0; vertex < numVerts; ++vertex) {
				const byte *st = file + surfaceOffset + static_cast<size_t>(ofsSt) + static_cast<size_t>(vertex) * VRHI_MD3_ST_BYTES;
				const float s = VRHI_MD3Float(st), t = VRHI_MD3Float(st + 4);
				if (!std::isfinite(s) || !std::isfinite(t)) return false;
				surface.st.emplace_back(s, t);
			}
			for (int frame = 0; frame < numFrames; ++frame) for (int vertex = 0; vertex < numVerts; ++vertex) {
				const byte *xyz = file + surfaceOffset + static_cast<size_t>(ofsXYZ) +
					(static_cast<size_t>(frame) * static_cast<size_t>(numVerts) + static_cast<size_t>(vertex)) * VRHI_MD3_XYZ_BYTES;
				VRHI_MD3XYZ value = { VRHI_MD3Short(xyz), VRHI_MD3Short(xyz + 2), VRHI_MD3Short(xyz + 4), VRHI_MD3Short(xyz + 6) };
				surface.xyz.push_back(value);
			}
			for (int triangle = 0; triangle < numTriangles; ++triangle) {
				const byte *tri = file + surfaceOffset + static_cast<size_t>(ofsTriangles) + static_cast<size_t>(triangle) * VRHI_MD3_TRIANGLE_BYTES;
				for (int corner = 0; corner < 3; ++corner) {
					const int32_t index = VRHI_MD3Int(tri + corner * 4);
					if (index < 0 || index >= numVerts) return false;
					surface.indexes.push_back(static_cast<uint32_t>(index));
				}
			}
			model.surfaces.push_back(std::move(surface));
			surfaceOffset += static_cast<size_t>(surfaceEndDisk);
		}
		if (surfaceOffset > end) return false;
		model.cpuBytes = static_cast<size_t>(numFrames) * VRHI_MD3_FRAME_BYTES + tagCount * VRHI_MD3_TAG_BYTES;
		for (const VRHI_MD3Surface &surface : model.surfaces) model.cpuBytes += surface.st.size() * sizeof(glm::vec2) + surface.xyz.size() * sizeof(VRHI_MD3XYZ) + surface.indexes.size() * sizeof(uint32_t);
		if (model.cpuBytes > VRHI_MAX_MD3_MEMORY || g_md3MemoryBytes > VRHI_MAX_MD3_MEMORY - model.cpuBytes) return false;
		*out = std::move(model);
		return true;
	} catch (...) {
		return false;
	}
}

static qhandle_t VRHI_RegisterModel(const char *name) {
	size_t length = 0;
	int inlineModelIndex = -1;
	// Inline names are resolved against the currently loaded BSP after the
	// handle is allocated. This permits the engine's normal register-before-
	// LoadWorld sequence while RefreshInlineModelHandles removes stale maps.
	if (VRHI_InlineModelName(name, &inlineModelIndex)) {
		const qhandle_t handle = VRHI_RegisterName(g_modelHandles, name, "RegisterModel");
		VRHI_RefreshInlineModelHandles();
		return handle;
	}
	// A leading '*' which is not a canonical bounded inline name is never an
	// MD3/model handle.
	if (name != nullptr && name[0] == '*') {
		VRHI_Printf(PRINT_DEVELOPER, "renderer_vrhi: RegisterModel('%s') rejected; qhandle 0\n",
			name);
		return 0;
	}
	// Preserve qhandle 0 semantics for invalid names exactly like the GL
	// renderers: NULL/empty/overlong and traversal-style names report "not
	// registered" instead of consuming a fake non-zero handle that mods
	// would treat as a real model.
	if (!VRHI_MD3SafeName(name, &length)) {
		VRHI_Printf(PRINT_DEVELOPER, "renderer_vrhi: RegisterModel('%s') rejected; qhandle 0\n",
			name != nullptr ? name : "(null)");
		return 0;
	}
	const qhandle_t handle = VRHI_RegisterName(g_modelHandles, name, "RegisterModel");
	if (g_md3ModelByHandle.find(handle) != g_md3ModelByHandle.end()) return handle;
	if (!VRHI_MD3Extension(name) || g_md3Models.size() >= VRHI_MAX_MD3_MODELS ||
		g_ri.FS_ReadFile == nullptr || g_ri.FS_FreeFile == nullptr) return handle;
	void *fileData = nullptr;
	const long fileSizeLong = g_ri.FS_ReadFile(name, &fileData);
	VRHI_MD3Model model;
	bool parsed = fileData != nullptr && fileSizeLong > 0 &&
		static_cast<unsigned long>(fileSizeLong) <= VRHI_MAX_MD3_FILE_BYTES &&
		VRHI_ParseMD3(static_cast<const byte *>(fileData), static_cast<size_t>(fileSizeLong), name, &model);
	if (fileData != nullptr) g_ri.FS_FreeFile(fileData);
	if (!parsed) {
		VRHI_Printf(PRINT_DEVELOPER, "renderer_vrhi: MD3 '%s' rejected; solid/no-op model handle %d\n", name, static_cast<int>(handle));
		return handle;
	}
	model.valid = true;
	g_md3MemoryBytes += model.cpuBytes;
	g_md3ModelByHandle.emplace(handle, g_md3Models.size());
	g_md3Models.push_back(std::move(model));
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: registered MD3 '%s' handle=%d frames=%d surfaces=%zu bytes=%zu\n", name, static_cast<int>(handle), g_md3Models.back().numFrames, g_md3Models.back().surfaces.size(), g_md3Models.back().cpuBytes);
	return handle;
}
static bool VRHI_SkinExtension(const char *name) {
	// GL parity: a name ending in ".skin" is parsed as a skin text file;
	// anything else registers the name itself as one inert surface shader.
	size_t length = 0;
	while (length < MAX_QPATH && name[length] != '\0') ++length;
	if (length < 5) return false;
	return std::strcmp(name + length - 5, ".skin") == 0;
}

static qhandle_t VRHI_RegisterSkin(const char *name) {
	// Preserve qhandle 0 semantics for invalid names exactly like the GL
	// renderers: NULL/empty/overlong and traversal-style names fail with 0.
	if (!VRHI_MD3SafeName(name, nullptr)) {
		VRHI_Printf(PRINT_DEVELOPER, "renderer_vrhi: RegisterSkin('%s') rejected; qhandle 0\n",
			name != nullptr ? name : "(null)");
		return 0;
	}
	// GL resolves skin names case-insensitively, so the handle/failure caches
	// are keyed by the lowercased name.
	const std::string key = VRHI_LowerASCII(name);
	const std::unordered_map<std::string, qhandle_t>::const_iterator cached =
		g_skinHandles.find(key);
	if (cached != g_skinHandles.end()) return cached->second;
	if (g_skinFailures.find(key) != g_skinFailures.end()) return 0;
	if (g_skins.size() >= static_cast<size_t>(VRHI_MAX_SKINS)) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: RegisterSkin('%s') skipped; skin count cap reached\n",
			name);
		return 0;
	}

	VRHI_Skin skin;
	skin.name = name;
	if (!VRHI_SkinExtension(name)) {
		// GL parity: a non-.skin name becomes a one-surface skin whose shader
		// is the name itself. The empty surface name never matches an MD3
		// surface, so the override is inert; the direct-image handle is still
		// warmed through the bounded shared registration path.
		VRHI_SkinSurface surf;
		surf.shader = VRHI_RegisterModelShader(name);
		skin.surfaces.push_back(surf);
	} else {
		// Parse bounded .skin text through FS_ReadFile/FS_FreeFile. No pointer
		// into the FS buffer is retained: every value is copied into bounded
		// strings before the file data is freed below.
		void *fileData = nullptr;
		const long fileSizeLong = g_ri.FS_ReadFile != nullptr
			? g_ri.FS_ReadFile(name, &fileData) : 0;
		if (fileData != nullptr && fileSizeLong > 0 &&
			static_cast<unsigned long>(fileSizeLong) <= VRHI_SKIN_MAX_FILE_BYTES) {
			std::vector<VRHI_SkinEntry> entries;
			if (VRHI_ParseSkinText(static_cast<const char *>(fileData),
				static_cast<size_t>(fileSizeLong), entries)) {
				for (const VRHI_SkinEntry &entry : entries) {
					// The parser already guaranteed non-empty, < MAX_QPATH,
					// lowercased surface names; re-validate the shader qpath so
					// hostile entries can never reach the image resolver. Each
					// valid shader path reuses the bounded direct image
					// registration/handle path; unsupported shader scripts or
					// materials keep the safe solid fallback via the same no-op
					// handle semantics as model shaders.
					if (!VRHI_MD3SafeName(entry.shader.c_str(), nullptr)) continue;
					if (skin.surfaces.size() >= VRHI_SKIN_MAX_SURFACES ||
						g_skinTotalEntries >= VRHI_MAX_SKIN_ENTRIES_TOTAL) break;
					VRHI_SkinSurface surf;
					surf.surface = entry.surface;
					surf.shader = VRHI_RegisterModelShader(entry.shader);
					skin.surfaces.push_back(std::move(surf));
					++g_skinTotalEntries;
				}
			}
		}
		if (fileData != nullptr && g_ri.FS_FreeFile != nullptr) g_ri.FS_FreeFile(fileData);
		if (skin.surfaces.empty()) {
			// GL returns 0 ("use the default skin") for a missing, empty, or
			// malformed .skin file; cache the failure for this session so
			// repeated registrations stay a bounded no-op.
			if (g_skinFailures.size() < static_cast<size_t>(VRHI_MAX_SKINS))
				g_skinFailures.emplace(key);
			VRHI_Printf(PRINT_DEVELOPER,
				"renderer_vrhi: RegisterSkin('%s') has no usable surfaces; default skin (qhandle 0)\n",
				name);
			return 0;
		}
	}

	const qhandle_t handle = static_cast<qhandle_t>(g_skins.size());
	g_skins.push_back(std::move(skin));
	g_skinHandles.emplace(key, handle);
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: registered skin '%s' handle=%d surfaces=%zu\n",
		name, static_cast<int>(handle), g_skins.back().surfaces.size());
	return handle;
}

// Case-insensitive customSkin lookup: returns the registered shader handle for
// the surface named `surfaceName`, or 0 when the skin or surface is absent so
// the caller falls back to the embedded surface shader.
static qhandle_t VRHI_SkinSurfaceShader(qhandle_t hSkin,
	const std::string &surfaceName) {
	if (hSkin <= 0 || static_cast<size_t>(hSkin) >= g_skins.size()) return 0;
	const VRHI_Skin &skin = g_skins[static_cast<size_t>(hSkin)];
	for (const VRHI_SkinSurface &entry : skin.surfaces) {
		if (entry.surface == surfaceName) return entry.shader;
	}
	return 0;
}
static qhandle_t VRHI_RegisterShader(const char *name) {
	const qhandle_t handle = VRHI_RegisterName(g_shaderHandles, name,
		"RegisterShader");
	if (handle != 0) VRHI_RegisterDirectUITexture(handle, name);
	return handle;
}
static qhandle_t VRHI_RegisterShaderNoMip(const char *name) {
	// RegisterShader and RegisterShaderNoMip share one handle space, mirroring
	// the GL renderers where both paths resolve through the same shader table.
	const qhandle_t handle = VRHI_RegisterName(g_shaderHandles, name,
		"RegisterShaderNoMip");
	if (handle != 0) VRHI_RegisterDirectUITexture(handle, name);
	return handle;
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

static std::string VRHI_LowerASCII(const std::string &text) {
	std::string lower = text;
	for (char &ch : lower) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return lower;
}

static bool VRHI_ResolveShaderScriptFile(const char *fileName,
	std::string *resolved) {
	if (resolved != nullptr) resolved->clear();
	if (fileName == nullptr || resolved == nullptr) return false;
	size_t length = 0;
	while (length < MAX_QPATH && fileName[length] != '\0') ++length;
	if (length == 0 || length >= MAX_QPATH) return false;
	const std::string name(fileName, length);
	if (!VRHI_ShaderPathIsSafe(name) || name.find('\\') != std::string::npos) return false;
	const std::string lower = VRHI_LowerASCII(name);
	if (lower.size() < 7 || lower.compare(lower.size() - 7, 7, ".shader") != 0 ||
		name.size() + 8 >= MAX_QPATH) return false;
	*resolved = "scripts/" + name;
	return true;
}

static void VRHI_ScanShaderScripts(const std::vector<std::string> &shaderNames) {
	g_worldShaderScriptPaths.clear();
	g_worldShaderScriptsScanned = true;
	std::unordered_map<std::string, bool> wanted;
	char **fileList = nullptr;
	void *activeFileData = nullptr;
	try {
		for (const std::string &name : shaderNames) {
			if (!name.empty()) {
				const std::string lower = VRHI_LowerASCII(name);
				wanted.emplace(lower, true);
				g_worldShaderScriptPaths.emplace(lower, std::string());
			}
		}
		if (g_ri.FS_ListFiles == nullptr || g_ri.FS_FreeFileList == nullptr ||
			g_ri.FS_ReadFile == nullptr || g_ri.FS_FreeFile == nullptr) return;
		int fileCount = 0;
		fileList = g_ri.FS_ListFiles("scripts", ".shader", &fileCount);
		if (fileList == nullptr) return;
		if (fileCount < 0 || fileCount > VRHI_MAX_SHADER_FILES) {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: shader script scan skipped; file count cap exceeded (%d)\n",
				fileCount);
			g_ri.FS_FreeFileList(fileList);
			return;
		}
		size_t totalText = 0;
		for (int i = 0; i < fileCount; ++i) {
			std::string qpath;
			if (!VRHI_ResolveShaderScriptFile(fileList[i], &qpath)) continue;
			activeFileData = nullptr;
			const long fileSizeLong = g_ri.FS_ReadFile(qpath.c_str(), &activeFileData);
			if (activeFileData == nullptr || fileSizeLong < 0) {
				if (activeFileData != nullptr) g_ri.FS_FreeFile(activeFileData);
				activeFileData = nullptr;
				continue;
			}
			bool parsed = false;
			try {
				const size_t fileSize = static_cast<size_t>(fileSizeLong);
				if (fileSize <= VRHI_MAX_SHADER_FILE_BYTES &&
					totalText <= VRHI_MAX_SHADER_TEXT_BYTES - fileSize) {
					totalText += fileSize;
					std::unordered_map<std::string, std::string> parsedResults;
					parsed = VRHI_ParseShaderScript(static_cast<const byte *>(activeFileData),
						fileSize, wanted, &parsedResults);
					if (parsed) {
						for (auto &result : parsedResults) {
							auto found = g_worldShaderScriptPaths.find(result.first);
							if (found != g_worldShaderScriptPaths.end() && found->second.empty()) {
								found->second = std::move(result.second);
							}
						}
					}
				} else {
					VRHI_Printf(PRINT_WARNING,
						"renderer_vrhi: shader script scan text cap reached at '%s'\n",
						qpath.c_str());
				}
			} catch (...) {
				parsed = false;
			}
			g_ri.FS_FreeFile(activeFileData);
			activeFileData = nullptr;
		}
		g_ri.FS_FreeFileList(fileList);
		fileList = nullptr;
	} catch (...) {
		if (activeFileData != nullptr && g_ri.FS_FreeFile != nullptr) {
			g_ri.FS_FreeFile(activeFileData);
			activeFileData = nullptr;
		}
		if (fileList != nullptr && g_ri.FS_FreeFileList != nullptr) {
			g_ri.FS_FreeFileList(fileList);
			fileList = nullptr;
		}
		// Keep a complete miss cache after allocation or malformed-input failure.
		g_worldShaderScriptPaths.clear();
		for (const std::string &name : shaderNames) {
			if (!name.empty()) g_worldShaderScriptPaths.emplace(VRHI_LowerASCII(name), std::string());
		}
	}
}

static bool VRHI_DecodeDirectImage(const char *name, int maxDimension,
	size_t maxBytes, std::string *pathOut, vrhi_image::DecodeResult *decodedOut) {
	if (pathOut != nullptr) pathOut->clear();
	if (decodedOut != nullptr) *decodedOut = vrhi_image::DecodeResult();
	std::string base;
	if (!VRHI_ResolveDirectImagePath(name, &base) ||
		g_ri.FS_ReadFile == nullptr || g_ri.FS_FreeFile == nullptr ||
		decodedOut == nullptr) return false;
	const size_t slash = base.find_last_of("/\\");
	const size_t dot = base.find_last_of('.');
	const bool explicitExtension = dot != std::string::npos &&
		(slash == std::string::npos || dot > slash) && dot + 1 < base.size();
	std::vector<std::string> candidates;
	if (explicitExtension) {
		candidates.push_back(base);
	} else {
		static const char *extensions[] = { ".tga", ".jpg", ".jpeg", ".png" };
		for (const char *extension : extensions) {
			if (base.size() + std::strlen(extension) >= MAX_QPATH) continue;
			candidates.emplace_back(base + extension);
		}
	}
	for (const std::string &path : candidates) {
		void *fileData = nullptr;
		const long fileSizeLong = g_ri.FS_ReadFile(path.c_str(), &fileData);
		if (fileData == nullptr || fileSizeLong <= 0) {
			if (fileData != nullptr) g_ri.FS_FreeFile(fileData);
			continue;
		}
		bool ok = false;
		try {
			const std::uint8_t *bytes = static_cast<const std::uint8_t *>(fileData);
			const size_t size = static_cast<size_t>(fileSizeLong);
			const size_t pathDot = path.find_last_of('.');
			std::string ext = pathDot == std::string::npos ? std::string() : path.substr(pathDot + 1);
			for (char &ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			if (ext == "tga") {
				vrhi_tga::DecodeResult tga;
				ok = vrhi_tga::Decode(bytes, size, maxDimension, maxBytes, &tga);
				if (ok) { decodedOut->ok = tga.ok; decodedOut->width = tga.width; decodedOut->height = tga.height; decodedOut->rgba = std::move(tga.rgba); }
			} else if (ext == "jpg" || ext == "jpeg") {
				ok = vrhi_image::DecodeJPEG(bytes, size, maxDimension, maxBytes, decodedOut);
			} else if (ext == "png") {
				ok = vrhi_image::DecodePNG(bytes, size, maxDimension, maxBytes, decodedOut);
			}
		} catch (...) { ok = false; }
		g_ri.FS_FreeFile(fileData);
		if (ok) {
			if (pathOut != nullptr) *pathOut = path;
			return true;
		}
	}
	return false;
}

static bool VRHI_LoadDiffuseTGA(const char *shaderName, int *imageIndex) {
	if (imageIndex != nullptr) *imageIndex = -1;
	std::string path;
	vrhi_image::DecodeResult decoded;
	bool scriptResolved = false;
	if (!VRHI_DecodeDirectImage(shaderName, VRHI_MAX_WORLD_DIFFUSE_DIMENSION,
		VRHI_MAX_WORLD_DIFFUSE_BYTES, &path, &decoded)) {
		std::string shaderKey;
		if (shaderName != nullptr) {
			size_t length = 0;
			while (length < MAX_QPATH && shaderName[length] != '\0') ++length;
			if (length > 0 && length < MAX_QPATH) shaderKey = VRHI_LowerASCII(
				std::string(shaderName, length));
		}
		const auto found = g_worldShaderScriptPaths.find(shaderKey);
		if (!g_worldShaderScriptsScanned || found == g_worldShaderScriptPaths.end() ||
			found->second.empty() || !VRHI_DecodeDirectImage(found->second.c_str(),
				VRHI_MAX_WORLD_DIFFUSE_DIMENSION, VRHI_MAX_WORLD_DIFFUSE_BYTES,
				&path, &decoded)) return false;
		scriptResolved = true;
	}
	for (size_t i = 0; i < g_worldDiffuseImages.size(); ++i) {
		if (g_worldDiffuseImages[i].path == path) {
			if (scriptResolved) g_worldDiffuseImages[i].scriptResolved = true;
			if (imageIndex != nullptr) *imageIndex = static_cast<int>(i);
			return true;
		}
	}
	if (g_worldDiffuseImages.size() >= VRHI_MAX_WORLD_DIFFUSE_IMAGES) return false;
	const size_t pixelBytes = decoded.rgba.size();
	size_t existingBytes = 0;
	for (const VRHI_WorldDiffuseImage &image : g_worldDiffuseImages) existingBytes += image.pixels.size();
	if (pixelBytes > VRHI_MAX_WORLD_DIFFUSE_BYTES ||
		existingBytes > VRHI_MAX_WORLD_DIFFUSE_BYTES - pixelBytes) return false;
	VRHI_WorldDiffuseImage image;
	image.path = path;
	image.width = decoded.width;
	image.height = decoded.height;
	image.scriptResolved = scriptResolved;
	image.pixels = std::move(decoded.rgba);
	g_worldDiffuseImages.push_back(std::move(image));
	const int index = static_cast<int>(g_worldDiffuseImages.size() - 1);
	if (imageIndex != nullptr) *imageIndex = index;
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: decoded %sBSP diffuse '%s' (%dx%d, %zu bytes)\n",
		scriptResolved ? "script-resolved " : "", path.c_str(), decoded.width,
		decoded.height, pixelBytes);
	return true;
}

static void VRHI_UploadUITextures(void) {
	if (!g_deviceInitialized || g_uiTexturedProgram.empty()) return;
	for (VRHI_UITexture &image : g_uiTextures) {
		if (image.texture != VRHI_INVALID_HANDLE || image.pixels.empty()) continue;
		vhTexture texture = vhAllocTexture();
		if (texture == VRHI_INVALID_HANDLE) {
			VRHI_Printf(PRINT_WARNING, "renderer_vrhi: UI image '%s' allocation failed; solid fallback\n", image.path.c_str());
			continue;
		}
		vhMem *data = new vhMem(image.pixels.size());
		std::memcpy(data->data(), image.pixels.data(), data->size());
		const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
		vhCreateTexture2D(texture, image.path.c_str(), glm::ivec2(image.width, image.height), 1,
			nvrhi::Format::RGBA8_UNORM, VRHI_TEXTURE_NONE | VRHI_SAMPLER_NONE, data);
		vhFinish();
		if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
			VRHI_Printf(PRINT_WARNING, "renderer_vrhi: UI image '%s' upload failed; solid fallback\n", image.path.c_str());
			vhDestroyTexture(texture);
			vhFinish();
			continue;
		}
		image.texture = texture;
		VRHI_Printf(PRINT_ALL, "renderer_vrhi: uploaded UI image '%s' (%dx%d, %zu bytes)\n",
			image.path.c_str(), image.width, image.height, image.pixels.size());
	}
}

static bool VRHI_RegisterDirectUITexture(qhandle_t handle, const char *name) {
	if (handle == 0 || g_uiTextureAttempts.find(handle) != g_uiTextureAttempts.end()) return false;
	g_uiTextureAttempts.emplace(handle, true);
	if (g_uiTextures.size() >= VRHI_MAX_UI_TEXTURES) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: UI shader '%s' skipped; texture count cap reached\n",
			name != nullptr ? name : "(null)");
		return false;
	}
	std::string path;
	vrhi_image::DecodeResult decoded;
	if (!VRHI_DecodeDirectImage(name, VRHI_MAX_UI_TEXTURE_DIMENSION,
		VRHI_MAX_UI_TEXTURE_BYTES, &path, &decoded)) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: UI shader '%s' uses solid-color fallback (only bounded TGA/JPG/PNG images are supported)\n",
			name != nullptr ? name : "(null)");
		return false;
	}
	size_t existingBytes = 0;
	for (const VRHI_UITexture &image : g_uiTextures) existingBytes += image.pixels.size();
	if (decoded.rgba.size() > VRHI_MAX_UI_TEXTURE_BYTES ||
		existingBytes > VRHI_MAX_UI_TEXTURE_BYTES - decoded.rgba.size()) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: UI image '%s' skipped; aggregate memory cap reached; solid fallback\n",
			path.c_str());
		return false;
	}
	VRHI_UITexture image;
	image.path = std::move(path);
	image.width = decoded.width;
	image.height = decoded.height;
	image.pixels = std::move(decoded.rgba);
	g_uiTextures.push_back(std::move(image));
	g_uiTextureByHandle.emplace(handle, g_uiTextures.size() - 1);
	VRHI_Printf(PRINT_ALL, "renderer_vrhi: registered UI image '%s' for shader handle %d\n",
		g_uiTextures.back().path.c_str(), static_cast<int>(handle));
	VRHI_UploadUITextures();
	return true;
}

struct VRHI_PatchMesh {
	std::vector<VRHI_WorldVertex> vertices;
	std::vector<uint32_t> indexes;
};

static bool VRHI_FinitePatchControl(const drawVert_t &vertex) {
	return std::isfinite(vertex.xyz[0]) && std::isfinite(vertex.xyz[1]) &&
		std::isfinite(vertex.xyz[2]) && std::isfinite(vertex.st[0]) &&
		std::isfinite(vertex.st[1]) && std::isfinite(vertex.lightmap[0]) &&
		std::isfinite(vertex.lightmap[1]);
}

// Decode and tessellate all overlapping quadratic 3x3 blocks in a patch.
// This helper owns only temporary vectors; the caller commits them to the
// world aggregate after every validation and cap check has succeeded.
static bool VRHI_TessellatePatch(const byte *vertsData, int vertCount,
	const dsurface_t &surface, VRHI_PatchMesh *mesh) {
	if (vertsData == nullptr || mesh == nullptr || surface.patchWidth < 3 ||
		surface.patchHeight < 3 || (surface.patchWidth & 1) == 0 ||
		(surface.patchHeight & 1) == 0 ||
		surface.patchWidth > VRHI_MAX_PATCH_DIMENSION ||
		surface.patchHeight > VRHI_MAX_PATCH_DIMENSION || surface.firstVert < 0 ||
		surface.numVerts < 0 || surface.firstVert > vertCount ||
		surface.numVerts > vertCount - surface.firstVert) return false;
	const size_t controlCount = static_cast<size_t>(surface.patchWidth) *
		static_cast<size_t>(surface.patchHeight);
	const int blocksX = (surface.patchWidth - 1) / 2;
	const int blocksY = (surface.patchHeight - 1) / 2;
	if (controlCount > static_cast<size_t>(VRHI_MAX_PATCH_CONTROL_VERTICES) ||
		controlCount != static_cast<size_t>(surface.numVerts) ||
		blocksX <= 0 || blocksY <= 0 ||
		static_cast<size_t>(blocksX) * static_cast<size_t>(blocksY) >
			static_cast<size_t>(VRHI_MAX_PATCH_BLOCKS)) return false;

	std::vector<drawVert_t> controls;
	try {
		controls.reserve(controlCount);
		for (size_t i = 0; i < controlCount; ++i) {
			const drawVert_t control = VRHI_ReadDrawVert(vertsData,
				surface.firstVert + static_cast<int>(i));
			if (!VRHI_FinitePatchControl(control)) return false;
			controls.push_back(control);
		}
		const size_t blockCount = static_cast<size_t>(blocksX) *
			static_cast<size_t>(blocksY);
		const size_t gridWidth = static_cast<size_t>(VRHI_PATCH_SUBDIVISIONS + 1);
		const size_t verticesPerPatch = blockCount * gridWidth * gridWidth;
		const size_t indexesPerPatch = blockCount *
			static_cast<size_t>(VRHI_PATCH_SUBDIVISIONS) *
			static_cast<size_t>(VRHI_PATCH_SUBDIVISIONS) * 6u;
		if (verticesPerPatch > VRHI_MAX_WORLD_VERTICES ||
			indexesPerPatch > VRHI_MAX_WORLD_INDEXES) return false;
		mesh->vertices.reserve(verticesPerPatch);
		mesh->indexes.reserve(indexesPerPatch);
		for (int blockY = 0; blockY < blocksY; ++blockY) {
			for (int blockX = 0; blockX < blocksX; ++blockX) {
				const int controlX = blockX * 2;
				const int controlY = blockY * 2;
				for (int y = 0; y <= VRHI_PATCH_SUBDIVISIONS; ++y) {
					const float v = static_cast<float>(y) /
						static_cast<float>(VRHI_PATCH_SUBDIVISIONS);
					const float by[3] = { (1.0f - v) * (1.0f - v),
						2.0f * v * (1.0f - v), v * v };
					for (int x = 0; x <= VRHI_PATCH_SUBDIVISIONS; ++x) {
						const float u = static_cast<float>(x) /
							static_cast<float>(VRHI_PATCH_SUBDIVISIONS);
						const float bx[3] = { (1.0f - u) * (1.0f - u),
							2.0f * u * (1.0f - u), u * u };
						VRHI_WorldVertex vertex;
						vertex.position = glm::vec3(0.0f);
						vertex.diffuse = glm::vec2(0.0f);
						vertex.lightmap = glm::vec2(0.0f);
						vertex.lightmapLayer = -1.0f;
						vertex.color = glm::vec4(1.0f);
						for (int row = 0; row < 3; ++row) {
							for (int column = 0; column < 3; ++column) {
								const drawVert_t &control = controls[static_cast<size_t>(controlY + row) *
									static_cast<size_t>(surface.patchWidth) + controlX + column];
								const float weight = bx[column] * by[row];
								vertex.position += weight * glm::vec3(control.xyz[0], control.xyz[1], control.xyz[2]);
								vertex.diffuse += weight * glm::vec2(control.st[0], control.st[1]);
								vertex.lightmap += weight * glm::vec2(control.lightmap[0], control.lightmap[1]);
							}
						}
						if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
							!std::isfinite(vertex.position.z) || !std::isfinite(vertex.diffuse.x) ||
							!std::isfinite(vertex.diffuse.y) || !std::isfinite(vertex.lightmap.x) ||
							!std::isfinite(vertex.lightmap.y)) return false;
						mesh->vertices.push_back(vertex);
					}
				}
				const uint32_t first = static_cast<uint32_t>(mesh->vertices.size() - gridWidth * gridWidth);
				for (int y = 0; y < VRHI_PATCH_SUBDIVISIONS; ++y) {
					for (int x = 0; x < VRHI_PATCH_SUBDIVISIONS; ++x) {
						const uint32_t a = first + static_cast<uint32_t>(y * (VRHI_PATCH_SUBDIVISIONS + 1) + x);
						const uint32_t b = a + 1;
						const uint32_t d = a + static_cast<uint32_t>(VRHI_PATCH_SUBDIVISIONS + 1);
						const uint32_t c = d + 1;
						const uint32_t triangles[2][3] = {{ a, b, c }, { a, c, d }};
						for (const auto &triangle : triangles) {
							const glm::vec3 edge1 = mesh->vertices[triangle[1]].position - mesh->vertices[triangle[0]].position;
							const glm::vec3 edge2 = mesh->vertices[triangle[2]].position - mesh->vertices[triangle[0]].position;
							const glm::vec3 cross = glm::cross(edge1, edge2);
							const float area = glm::dot(cross, cross);
							if (!std::isfinite(cross.x) || !std::isfinite(cross.y) ||
								!std::isfinite(cross.z) || !std::isfinite(area) || area <= 1.0e-10f) continue;
							mesh->indexes.push_back(triangle[0]);
							mesh->indexes.push_back(triangle[1]);
							mesh->indexes.push_back(triangle[2]);
						}
					}
				}
			}
		}
	} catch (...) {
		mesh->vertices.clear();
		mesh->indexes.clear();
		return false;
	}
	return !mesh->vertices.empty() && !mesh->indexes.empty();
}

static bool VRHI_BuildInlineSurface(const byte *vertsData, int vertCount,
	const byte *indexesData, int indexCount, const dsurface_t &surface,
	int surfaceLightmapLayer, int diffuseImage, VRHI_InlineBSPModel *model) {
	if (model == nullptr || vertsData == nullptr || indexesData == nullptr ||
		surface.numVerts < 3 || surface.numVerts > static_cast<int>(VRHI_MAX_INLINE_SURFACE_VERTICES) ||
		surface.firstVert < 0 || surface.firstVert > vertCount ||
		surface.numVerts > vertCount - surface.firstVert) return false;
	try {
		VRHI_PatchMesh patch;
		if (surface.surfaceType == MST_PATCH) {
			if (model->batches.size() >= VRHI_MAX_INLINE_BATCHES ||
				!VRHI_TessellatePatch(vertsData, vertCount, surface, &patch) ||
			patch.vertices.size() > VRHI_MAX_INLINE_SURFACE_VERTICES ||
			patch.indexes.size() > VRHI_MAX_INLINE_SURFACE_INDEXES) return false;
			VRHI_WorldBatch batch;
			batch.firstIndex = static_cast<uint32_t>(model->indexes.size());
			batch.indexCount = static_cast<uint32_t>(patch.indexes.size());
			batch.diffuseImage = diffuseImage;
			for (VRHI_WorldVertex vertex : patch.vertices) {
				const bool lightmapped = surfaceLightmapLayer >= 0 &&
					vertex.lightmap.x >= 0.0f && vertex.lightmap.x <= 1.0f &&
					vertex.lightmap.y >= 0.0f && vertex.lightmap.y <= 1.0f;
				vertex.diffuse = diffuseImage >= 0 ? vertex.diffuse : glm::vec2(0.0f);
				vertex.lightmapLayer = lightmapped ? static_cast<float>(surfaceLightmapLayer) : -1.0f;
				vertex.color = glm::vec4(1.0f);
				model->vertices.push_back(vertex);
			}
			for (uint32_t index : patch.indexes) model->indexes.push_back(index +
				static_cast<uint32_t>(model->vertices.size() - patch.vertices.size()));
			model->batches.push_back(batch);
			return true;
		}
		if ((surface.surfaceType != MST_PLANAR && surface.surfaceType != MST_TRIANGLE_SOUP) ||
			surface.firstIndex < 0 || surface.numIndexes < 3 ||
			surface.numIndexes > static_cast<int>(VRHI_MAX_INLINE_SURFACE_INDEXES) ||
			surface.firstIndex > indexCount || surface.numIndexes > indexCount - surface.firstIndex ||
			surface.numIndexes % 3 != 0) return false;
		std::unordered_map<int, uint32_t> localVertices;
		localVertices.reserve(static_cast<size_t>(surface.numVerts));
		std::vector<VRHI_WorldVertex> vertices;
		std::vector<uint32_t> indexes;
		vertices.reserve(static_cast<size_t>(surface.numVerts));
		indexes.reserve(static_cast<size_t>(surface.numIndexes));
		for (int i = 0; i < surface.numIndexes; i += 3) {
			int source[3];
			for (int corner = 0; corner < 3; ++corner)
				source[corner] = VRHI_ReadIndex(indexesData, surface.firstIndex + i + corner);
			if (source[0] < 0 || source[1] < 0 || source[2] < 0 ||
				source[0] >= surface.numVerts || source[1] >= surface.numVerts || source[2] >= surface.numVerts ||
				source[0] == source[1] || source[0] == source[2] || source[1] == source[2]) continue;
			drawVert_t sourceVertices[3];
			glm::vec3 positions[3];
			bool valid = true;
			for (int corner = 0; corner < 3; ++corner) {
				sourceVertices[corner] = VRHI_ReadDrawVert(vertsData, surface.firstVert + source[corner]);
				positions[corner] = glm::vec3(sourceVertices[corner].xyz[0], sourceVertices[corner].xyz[1], sourceVertices[corner].xyz[2]);
				if (!VRHI_FinitePatchControl(sourceVertices[corner])) { valid = false; break; }
			}
			if (!valid) continue;
			const glm::vec3 cross = glm::cross(positions[1] - positions[0], positions[2] - positions[0]);
			if (!std::isfinite(cross.x) || !std::isfinite(cross.y) || !std::isfinite(cross.z) ||
				glm::dot(cross, cross) <= 1.0e-10f) continue;
			for (int corner = 0; corner < 3; ++corner) {
				auto found = localVertices.find(source[corner]);
				uint32_t local = 0;
				if (found == localVertices.end()) {
					if (vertices.size() >= VRHI_MAX_INLINE_SURFACE_VERTICES) return false;
					local = static_cast<uint32_t>(vertices.size());
					localVertices.emplace(source[corner], local);
					const bool lightmapped = surfaceLightmapLayer >= 0 &&
						sourceVertices[corner].lightmap[0] >= 0.0f && sourceVertices[corner].lightmap[0] <= 1.0f &&
						sourceVertices[corner].lightmap[1] >= 0.0f && sourceVertices[corner].lightmap[1] <= 1.0f;
					VRHI_WorldVertex vertex;
					vertex.position = positions[corner];
					vertex.diffuse = diffuseImage >= 0 ? glm::vec2(sourceVertices[corner].st[0], sourceVertices[corner].st[1]) : glm::vec2(0.0f);
					vertex.lightmap = lightmapped ? glm::vec2(sourceVertices[corner].lightmap[0], sourceVertices[corner].lightmap[1]) : glm::vec2(0.0f);
					vertex.lightmapLayer = lightmapped ? static_cast<float>(surfaceLightmapLayer) : -1.0f;
					vertex.color = glm::vec4(1.0f);
					vertices.push_back(vertex);
				} else local = found->second;
				indexes.push_back(local);
			}
		}
		if (indexes.empty() || model->batches.size() >= VRHI_MAX_INLINE_BATCHES) return false;
		VRHI_WorldBatch batch;
		batch.firstIndex = static_cast<uint32_t>(model->indexes.size());
		batch.indexCount = static_cast<uint32_t>(indexes.size());
		batch.diffuseImage = diffuseImage;
		const uint32_t baseVertex = static_cast<uint32_t>(model->vertices.size());
		model->vertices.insert(model->vertices.end(), vertices.begin(), vertices.end());
		for (uint32_t index : indexes) model->indexes.push_back(baseVertex + index);
		model->batches.push_back(batch);
		return true;
	} catch (...) {
		return false;
	}
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
		static_cast<size_t>(modelsLump.filelen) % sizeof(dmodel_t) != 0 ||
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
	const int modelCount = modelsLump.filelen / static_cast<int>(sizeof(dmodel_t));
	const int inlineModelCount = std::min(modelCount, VRHI_MAX_INLINE_MODELS + 1);
	if (modelCount > inlineModelCount) {
		VRHI_Printf(PRINT_WARNING, "renderer_vrhi: BSP world '%s' has %d models; inline model cap keeps first %d\n",
			name, modelCount, inlineModelCount);
	}
	const int surfaceCount = surfacesLump.filelen / static_cast<int>(sizeof(dsurface_t));
	const int vertCount = vertsLump.filelen / static_cast<int>(sizeof(drawVert_t));
	const int indexCount = indexesLump.filelen / static_cast<int>(sizeof(int));
	const int shaderCount = shadersLump.filelen / static_cast<int>(sizeof(dshader_t));
	if (surfaceCount > VRHI_MAX_WORLD_SURFACES) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: BSP world '%s' has too many surfaces; geometry rejected\n", name);
		g_ri.FS_FreeFile(fileData);
		return;
	}
	std::vector<std::string> bspShaderNames;
	try {
		bspShaderNames.reserve(static_cast<size_t>(shaderCount));
		for (int shaderIndex = 0; shaderIndex < shaderCount; ++shaderIndex) {
			const dshader_t shader = VRHI_ReadShader(shadersData, shaderIndex);
			size_t length = 0;
			while (length < MAX_QPATH && shader.shader[length] != '\0') ++length;
			if (length > 0 && length < MAX_QPATH) {
				bspShaderNames.emplace_back(shader.shader, length);
			}
		}
	} catch (...) {
		bspShaderNames.clear();
	}
	// Resolve all BSP shader-script candidates once for this map. Surface
	// batches only consult this copied cache; they never rescan scripts.
	VRHI_ScanShaderScripts(bspShaderNames);
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
		if ((surface.surfaceType != MST_PLANAR && surface.surfaceType != MST_PATCH &&
			surface.surfaceType != MST_TRIANGLE_SOUP) || surface.shaderNum < 0 ||
			surface.shaderNum >= shaderCount || (shader.surfaceFlags & (SURF_SKY | SURF_NODRAW)) ||
			surface.firstVert < 0 || surface.numVerts < 3 || surface.firstVert > vertCount ||
			surface.numVerts > vertCount - surface.firstVert ||
			surface.numVerts > VRHI_MAX_WORLD_SURFACE_VERTICES) {
			skipped++;
			continue;
		}
		const int surfaceLightmapLayer = lightmapLumpValid &&
			(shader.surfaceFlags & SURF_NOLIGHTMAP) == 0 &&
			surface.lightmapNum >= 0 && surface.lightmapNum < g_worldLightmapLayers
			? surface.lightmapNum : -1;
		int diffuseImage = -1;
		// Direct image probing remains first. If it misses, this map-scoped cache contains
		// only a first-stage script map/clampmap image candidate; all other shader
		// semantics retain the lightmap/solid fallback.
		VRHI_LoadDiffuseTGA(shader.shader, &diffuseImage);
		if (surface.surfaceType == MST_PATCH) {
			VRHI_PatchMesh patch;
			if (!VRHI_TessellatePatch(vertsData, vertCount, surface, &patch) ||
				g_worldBatches.size() >= VRHI_MAX_WORLD_BATCHES ||
				g_worldVertices.size() > VRHI_MAX_WORLD_VERTICES - patch.vertices.size() ||
				g_worldIndexes.size() > VRHI_MAX_WORLD_INDEXES - patch.indexes.size()) {
				VRHI_Printf(PRINT_WARNING,
					"renderer_vrhi: BSP patch surface %d rejected by validation/caps\n", surfaceIndex);
				skipped++;
				continue;
			}
			const uint32_t baseVertex = static_cast<uint32_t>(g_worldVertices.size());
			for (VRHI_WorldVertex vertex : patch.vertices) {
				const bool validLightmapUV = surfaceLightmapLayer >= 0 &&
					vertex.lightmap.x >= 0.0f && vertex.lightmap.x <= 1.0f &&
					vertex.lightmap.y >= 0.0f && vertex.lightmap.y <= 1.0f;
				vertex.diffuse = diffuseImage >= 0 ? vertex.diffuse : glm::vec2(0.0f);
				vertex.lightmapLayer = validLightmapUV ? static_cast<float>(surfaceLightmapLayer) : -1.0f;
				vertex.color = glm::vec4(1.0f);
				g_worldVertices.push_back(vertex);
				g_worldPositions.push_back(vertex.position);
			}
			const uint32_t firstIndex = static_cast<uint32_t>(g_worldIndexes.size());
			for (uint32_t index : patch.indexes) g_worldIndexes.push_back(baseVertex + index);
			VRHI_WorldBatch batch;
			batch.firstIndex = firstIndex;
			batch.indexCount = static_cast<uint32_t>(patch.indexes.size());
			batch.diffuseImage = diffuseImage;
			g_worldBatches.push_back(batch);
			g_worldSurfaceBatch[surfaceIndex] = static_cast<int32_t>(g_worldBatches.size() - 1);
			accepted++;
			continue;
		}
		if (surface.firstIndex < 0 || surface.numIndexes < 3 ||
			surface.numIndexes > VRHI_MAX_WORLD_SURFACE_INDEXES ||
			surface.firstIndex > indexCount || surface.numIndexes > indexCount - surface.firstIndex ||
			surface.numIndexes % 3 != 0) {
			skipped++;
			continue;
		}
		if (g_worldBatches.size() >= VRHI_MAX_WORLD_BATCHES ||
			g_worldVertices.size() > VRHI_MAX_WORLD_VERTICES -
				static_cast<size_t>(surface.numVerts) ||
			g_worldIndexes.size() > VRHI_MAX_WORLD_INDEXES -
				static_cast<size_t>(surface.numIndexes)) {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: BSP surface %d rejected; aggregate geometry cap reached\n",
				surfaceIndex);
			skipped++;
			continue;
		}
		std::unordered_map<int, uint32_t> localVertices;
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
					worldVertex.color = glm::vec4(1.0f);
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

	// Inline BSP models share the already decoded map images/lightmaps, but
	// retain their own local geometry so entities can apply origin/axis without
	// touching the static world upload. Build each surface atomically and keep
	// both per-model and aggregate caps independent of the world budget.
	g_inlineBSPModels.clear();
	g_inlineBSPModels.resize(static_cast<size_t>(inlineModelCount));
	size_t inlineVertices = 0;
	size_t inlineIndexes = 0;
	size_t inlineBatches = 0;
	int inlineAccepted = 0;
	int inlineSkipped = 0;
	for (int modelIndex = 1; modelIndex < inlineModelCount; ++modelIndex) {
		const dmodel_t inlineDisk = VRHI_ReadModel(modelsData, modelIndex);
		VRHI_InlineBSPModel &inlineModel = g_inlineBSPModels[static_cast<size_t>(modelIndex)];
		inlineModel.mins = glm::vec3(inlineDisk.mins[0], inlineDisk.mins[1], inlineDisk.mins[2]);
		inlineModel.maxs = glm::vec3(inlineDisk.maxs[0], inlineDisk.maxs[1], inlineDisk.maxs[2]);
		const bool validBounds = std::isfinite(inlineModel.mins.x) && std::isfinite(inlineModel.mins.y) &&
			std::isfinite(inlineModel.mins.z) && std::isfinite(inlineModel.maxs.x) &&
			std::isfinite(inlineModel.maxs.y) && std::isfinite(inlineModel.maxs.z) &&
			inlineModel.mins.x <= inlineModel.maxs.x && inlineModel.mins.y <= inlineModel.maxs.y &&
			inlineModel.mins.z <= inlineModel.maxs.z;
		if (!validBounds || inlineDisk.firstSurface < 0 || inlineDisk.numSurfaces <= 0 ||
			inlineDisk.firstSurface > surfaceCount || inlineDisk.numSurfaces > surfaceCount - inlineDisk.firstSurface) {
			++inlineSkipped;
			continue;
		}
		for (int surfaceIndex = inlineDisk.firstSurface;
			surfaceIndex < inlineDisk.firstSurface + inlineDisk.numSurfaces; ++surfaceIndex) {
			const dsurface_t surface = VRHI_ReadSurface(surfacesData, surfaceIndex);
			const dshader_t shader = surface.shaderNum >= 0 && surface.shaderNum < shaderCount
				? VRHI_ReadShader(shadersData, surface.shaderNum) : dshader_t();
			if ((surface.surfaceType != MST_PLANAR && surface.surfaceType != MST_PATCH &&
				surface.surfaceType != MST_TRIANGLE_SOUP) || surface.shaderNum < 0 ||
				surface.shaderNum >= shaderCount || (shader.surfaceFlags & (SURF_SKY | SURF_NODRAW)) ||
				surface.firstVert < 0 || surface.numVerts < 3 || surface.firstVert > vertCount ||
				surface.numVerts > vertCount - surface.firstVert ||
				surface.numVerts > VRHI_MAX_WORLD_SURFACE_VERTICES) {
				continue;
			}
			const int surfaceLightmapLayer = lightmapLumpValid &&
				(shader.surfaceFlags & SURF_NOLIGHTMAP) == 0 && surface.lightmapNum >= 0 &&
				surface.lightmapNum < g_worldLightmapLayers ? surface.lightmapNum : -1;
			int diffuseImage = -1;
			VRHI_LoadDiffuseTGA(shader.shader, &diffuseImage);
			VRHI_InlineBSPModel surfaceModel;
			if (!VRHI_BuildInlineSurface(vertsData, vertCount, indexesData, indexCount,
				surface, surfaceLightmapLayer, diffuseImage, &surfaceModel) ||
				surfaceModel.vertices.empty() || surfaceModel.indexes.empty() ||
				surfaceModel.batches.empty()) continue;
			if (inlineModel.vertices.size() > VRHI_MAX_INLINE_VERTICES - surfaceModel.vertices.size() ||
				inlineModel.indexes.size() > VRHI_MAX_INLINE_INDEXES - surfaceModel.indexes.size() ||
				inlineModel.batches.size() > VRHI_MAX_INLINE_BATCHES - surfaceModel.batches.size() ||
				inlineVertices > VRHI_MAX_INLINE_VERTICES - surfaceModel.vertices.size() ||
				inlineIndexes > VRHI_MAX_INLINE_INDEXES - surfaceModel.indexes.size() ||
				inlineBatches > VRHI_MAX_INLINE_BATCHES - surfaceModel.batches.size()) {
				++inlineSkipped;
				continue;
			}
			const uint32_t vertexBase = static_cast<uint32_t>(inlineModel.vertices.size());
			const uint32_t indexBase = static_cast<uint32_t>(inlineModel.indexes.size());
			inlineModel.vertices.insert(inlineModel.vertices.end(), surfaceModel.vertices.begin(), surfaceModel.vertices.end());
			for (uint32_t index : surfaceModel.indexes) inlineModel.indexes.push_back(vertexBase + index);
			for (VRHI_WorldBatch batch : surfaceModel.batches) {
				batch.firstIndex += indexBase;
				inlineModel.batches.push_back(batch);
			}
			inlineVertices += surfaceModel.vertices.size();
			inlineIndexes += surfaceModel.indexes.size();
			inlineBatches += surfaceModel.batches.size();
		}
		if (!inlineModel.vertices.empty() && !inlineModel.indexes.empty() && !inlineModel.batches.empty()) {
			inlineModel.valid = true;
			++inlineAccepted;
		} else {
			++inlineSkipped;
		}
	}
	VRHI_RefreshInlineModelHandles();
	g_worldLoaded = !g_worldVertices.empty() && !g_worldIndexes.empty();
	// Per-batch visibility stamps are rebuilt whenever the batch list changes.
	g_worldBatchMarked.assign(g_worldBatches.size(), 0);
	g_worldVisEpoch = 0;
	VRHI_Printf(PRINT_ALL,
		"renderer_vrhi: BSP world '%s': models=%d inlineAccepted=%d inlineSkipped=%d surfaces=%d accepted=%d skipped=%d vertices=%zu indexes=%zu lightmaps=%d diffuse=%zu batches=%zu cull=%s planes=%d nodes=%d leafs=%d leafsurfaces=%d clusters=%d vis=%s\n",
		name, modelCount, inlineAccepted, inlineSkipped, model.numSurfaces, accepted, skipped, g_worldVertices.size(), g_worldIndexes.size(),
		g_worldLightmapLayers, g_worldDiffuseImages.size(), g_worldBatches.size(),
		cullLumpsValid ? (g_worldVisAvailable ? "pvs" : "fallback-novis") : "fallback-lumps",
		planeCount, nodeCount, leafCount, leafSurfaceCount, g_worldNumClusters,
		g_worldVisAvailable ? "yes" : "no");
	g_ri.FS_FreeFile(fileData);
	if (g_worldLoaded && g_deviceInitialized) {
		VRHI_UploadWorldGeometry();
	} else if (inlineAccepted > 0 && g_deviceInitialized && VRHI_InitializeWorldShader()) {
		// A malformed/empty world model must not prevent an otherwise valid
		// inline entity from using the map's shared image/lightmap resources.
		VRHI_UploadWorldLightmaps();
		VRHI_UploadWorldDiffuse();
	}
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
static void VRHI_ClearScene(void) {
	VRHI_ReserveSceneStorage();
	g_sceneEntities.clear();
	g_scenePolys.clear();
	g_scenePolyVerts.clear();
	g_sceneVertices.clear();
	g_sceneIndexes.clear();
	g_sceneDraws.clear();
	g_sceneLights.clear();
	g_sceneModelDraws = 0;
}
static void VRHI_AddRefEntityToScene(const refEntity_t *entity) {
	if (entity == nullptr || !VRHI_FiniteEntity(*entity)) {
		VRHI_Printf(PRINT_DEVELOPER, "renderer_vrhi: dropped non-finite refEntity\n");
		return;
	}
	if (entity->reType < 0 || entity->reType >= RT_MAX_REF_ENTITY_TYPE) return;
	// RT_MODEL is handled during BuildSceneGeometry for MD3 and mapped inline
	// BSP '*N' handles; BSP model 0 remains on the static world path.
	// RT_LIGHTNING/RT_RAIL_CORE/RT_RAIL_RINGS ride the bounded camera-facing
	// beam-quad path used by RT_BEAM (see VRHI_BeamFallbackSupported).
	if (entity->reType != RT_MODEL && entity->reType != RT_SPRITE &&
		!VRHI_BeamFallbackSupported(static_cast<int>(entity->reType))) {
		// Report each unsupported type once per renderer lifetime instead of
		// once per entity per frame; model entities dominate real scenes and
		// would otherwise flood developer-mode console output.
		static bool reported[RT_MAX_REF_ENTITY_TYPE] = {};
		if (!reported[entity->reType]) {
			reported[entity->reType] = true;
			VRHI_Printf(PRINT_DEVELOPER,
				"renderer_vrhi: unsupported refEntity type %d dropped (RT_MODEL and complex effects are safe no-ops)\n",
				static_cast<int>(entity->reType));
		}
		return;
	}
	VRHI_ReserveSceneStorage();
	if (g_sceneEntities.size() >= VRHI_MAX_SCENE_ENTITIES) return;
	VRHI_SceneEntity submission;
	submission.entity = *entity;
	g_sceneEntities.push_back(submission);
}
static void VRHI_AddPolyToScene(qhandle_t shader, int numVerts,
	const polyVert_t *verts, int num) {
	if (verts == nullptr || numVerts < 3 || num <= 0 ||
		static_cast<size_t>(numVerts) > VRHI_MAX_SCENE_POLY_VERTICES ||
		static_cast<size_t>(num) > VRHI_MAX_SCENE_POLYS ||
		static_cast<size_t>(numVerts) > VRHI_MAX_SCENE_POLY_VERTICES /
			static_cast<size_t>(num)) return;
	VRHI_ReserveSceneStorage();
	for (int polyIndex = 0; polyIndex < num; ++polyIndex) {
		if (g_scenePolys.size() >= VRHI_MAX_SCENE_POLYS ||
			g_scenePolyVerts.size() > VRHI_MAX_SCENE_POLY_VERTICES -
				static_cast<size_t>(numVerts)) return;
		const polyVert_t *source = verts + static_cast<size_t>(polyIndex) * numVerts;
		for (int i = 0; i < numVerts; ++i) {
			if (!VRHI_FiniteVec3(source[i].xyz) || !std::isfinite(source[i].st[0]) ||
				!std::isfinite(source[i].st[1])) return;
		}
		VRHI_ScenePoly submission;
		submission.shader = shader;
		submission.firstVertex = static_cast<uint32_t>(g_scenePolyVerts.size());
		submission.numVerts = static_cast<uint32_t>(numVerts);
		g_scenePolyVerts.insert(g_scenePolyVerts.end(), source, source + numVerts);
		g_scenePolys.push_back(submission);
	}
}
// Bounded additive modulation for one world-space position. Returns the
// per-channel multiplier to apply to a base vertex color: 1 + add, where add
// comes from every submitted light with finite-distance falloff and is
// clamped to VRHI_DLIGHT_ADD_CAP. With no lights this returns exactly (1,1,1)
// so callers are strict no-ops.
static glm::vec3 VRHI_DLightModulation(const glm::vec3 &position) {
	if (g_sceneLights.empty()) return glm::vec3(1.0f);
	float add[3] = { 0.0f, 0.0f, 0.0f };
	VRHI_DLightAdd(g_sceneLights.data(), g_sceneLights.size(),
		position.x, position.y, position.z, add);
	return glm::vec3(1.0f + add[0], 1.0f + add[1], 1.0f + add[2]);
}

static int VRHI_LightForPoint(vec3_t point, vec3_t ambientLight,
	vec3_t directedLight, vec3_t lightDir) {
	// Always zero every output first so a failed query can never leak stale
	// or non-finite values into the caller.
	if (ambientLight != nullptr) {
		std::memset(ambientLight, 0, sizeof(vec3_t));
	}
	if (directedLight != nullptr) {
		std::memset(directedLight, 0, sizeof(vec3_t));
	}
	if (lightDir != nullptr) {
		std::memset(lightDir, 0, sizeof(vec3_t));
	}
	// No light grid exists in this renderer; the query is answered from the
	// submitted per-scene dynamic lights only, so no lights (or a non-finite
	// query point) is the safe qfalse fallback.
	if (point == nullptr || !VRHI_FiniteVec3(point) || g_sceneLights.empty()) {
		return qfalse;
	}
	float add[3] = { 0.0f, 0.0f, 0.0f };
	VRHI_DLightAdd(g_sceneLights.data(), g_sceneLights.size(),
		point[0], point[1], point[2], add);
	if (add[0] <= 0.0f && add[1] <= 0.0f && add[2] <= 0.0f) {
		return qfalse;
	}
	if (directedLight != nullptr) {
		// Byte scale matching the light-grid contract of the GL renderers;
		// add is capped to [0,1] so 255 is full dynamic-light brightness.
		directedLight[0] = add[0] * 255.0f;
		directedLight[1] = add[1] * 255.0f;
		directedLight[2] = add[2] * 255.0f;
	}
	if (lightDir != nullptr) {
		// Falloff-weighted direction toward the contributing lights. A zero
		// direction (query exactly on a light origin, or all lights at the
		// query point) leaves the output zeroed instead of emitting a
		// non-finite direction.
		glm::vec3 direction(0.0f);
		for (const VRHI_DLight &light : g_sceneLights) {
			const float falloff = VRHI_DLightFalloff(light,
				point[0], point[1], point[2]);
			if (falloff <= 0.0f) continue;
			const glm::vec3 delta(light.origin[0] - point[0],
				light.origin[1] - point[1], light.origin[2] - point[2]);
			const float distSquared = glm::dot(delta, delta);
			if (!std::isfinite(distSquared) || distSquared <= 1.0e-6f) continue;
			const float dist = std::sqrt(distSquared);
			if (!std::isfinite(dist) || dist <= 1.0e-6f) continue;
			direction += delta * (falloff / dist);
		}
		const float length = glm::length(direction);
		if (length > 1.0e-6f && std::isfinite(length)) {
			lightDir[0] = direction.x / length;
			lightDir[1] = direction.y / length;
			lightDir[2] = direction.z / length;
		}
	}
	return qtrue;
}

// Validates one dynamic light submission and stores it under the strict
// MAX_DLIGHTS cap. Non-finite origins/colors/intensity, non-positive
// intensity, and negative color channels are rejected; color channels above
// 1.0 are clamped so modulation stays bounded. additive is retained for API
// parity: vertex modulation treats both kinds identically (both add light).
static void VRHI_AddLightToSceneCommon(const vec3_t org, float intensity,
	float r, float g, float b, bool additive) {
	if (org == nullptr || !VRHI_FiniteVec3(org) || !std::isfinite(intensity) ||
		intensity <= 0.0f || !std::isfinite(r) || !std::isfinite(g) ||
		!std::isfinite(b) || r < 0.0f || g < 0.0f || b < 0.0f) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: dropped invalid dynamic light (non-finite or non-positive)\n");
		return;
	}
	VRHI_ReserveSceneStorage();
	if (g_sceneLights.size() >= VRHI_MAX_SCENE_LIGHTS) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: dynamic light dropped at MAX_DLIGHTS cap (%zu)\n",
			static_cast<size_t>(VRHI_MAX_SCENE_LIGHTS));
		return;
	}
	VRHI_DLight light;
	light.origin[0] = org[0];
	light.origin[1] = org[1];
	light.origin[2] = org[2];
	light.color[0] = r > 1.0f ? 1.0f : r;
	light.color[1] = g > 1.0f ? 1.0f : g;
	light.color[2] = b > 1.0f ? 1.0f : b;
	light.radius = intensity;
	light.additive = additive;
	g_sceneLights.push_back(light);
}

static void VRHI_AddLightToScene(const vec3_t org, float intensity,
	float r, float g, float b) {
	VRHI_AddLightToSceneCommon(org, intensity, r, g, b, false);
}

static void VRHI_AddAdditiveLightToScene(const vec3_t org, float intensity,
	float r, float g, float b) {
	VRHI_AddLightToSceneCommon(org, intensity, r, g, b, true);
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

static bool VRHI_FiniteVec3(const float *v) {
	return v != nullptr && std::isfinite(v[0]) && std::isfinite(v[1]) &&
		std::isfinite(v[2]);
}

static bool VRHI_FiniteEntity(const refEntity_t &entity) {
	if (!VRHI_FiniteVec3(entity.origin) || !VRHI_FiniteVec3(entity.oldorigin) ||
		!VRHI_FiniteVec3(entity.lightingOrigin) || !std::isfinite(entity.shadowPlane) ||
		!std::isfinite(entity.backlerp) || !std::isfinite(entity.shaderTexCoord[0]) ||
		!std::isfinite(entity.shaderTexCoord[1]) ||
		!std::isfinite(entity.shaderTime) || !std::isfinite(entity.radius) ||
		!std::isfinite(entity.rotation)) return false;
	for (int i = 0; i < 3; ++i) {
		if (!VRHI_FiniteVec3(entity.axis[i])) return false;
	}
	return true;
}

static VRHI_WorldVertex VRHI_SceneVertex(const glm::vec3 &position,
	const glm::vec2 &uv, const glm::vec4 &color) {
	VRHI_WorldVertex vertex;
	vertex.position = position;
	vertex.diffuse = uv;
	vertex.lightmap = glm::vec2(0.0f);
	vertex.lightmapLayer = -1.0f;
	vertex.color = color;
	// The same bounded dynamic-light modulation that updates the static world
	// vertex colors also modulates generated sprite/beam/poly/MD3 scene
	// vertices; with no submitted lights this is a strict no-op.
	if (!g_sceneLights.empty()) {
		const glm::vec3 modulation = VRHI_DLightModulation(position);
		vertex.color = glm::vec4(color.r * modulation.r, color.g * modulation.g,
			color.b * modulation.b, color.a);
	}
	return vertex;
}

static bool VRHI_AppendSceneDraw(qhandle_t shader, size_t firstIndex,
	size_t indexCount, int diffuseImage = -1, bool lightmapped = false) {
	if (indexCount == 0 || firstIndex > UINT32_MAX || indexCount > UINT32_MAX ||
		g_sceneDraws.size() >= VRHI_MAX_SCENE_DRAWS) return false;
	VRHI_SceneDraw draw;
	draw.firstIndex = static_cast<uint32_t>(firstIndex);
	draw.indexCount = static_cast<uint32_t>(indexCount);
	draw.shader = shader;
	draw.diffuseImage = diffuseImage;
	draw.lightmapped = lightmapped;
	g_sceneDraws.push_back(draw);
	return true;
}

static vhTexture VRHI_SceneTexture(const VRHI_SceneDraw &draw) {
	if (draw.diffuseImage >= 0 && static_cast<size_t>(draw.diffuseImage) < g_worldDiffuseImages.size())
		return g_worldDiffuseImages[static_cast<size_t>(draw.diffuseImage)].texture;
	const std::unordered_map<qhandle_t, size_t>::const_iterator found =
		g_uiTextureByHandle.find(draw.shader);
	if (found == g_uiTextureByHandle.end() || found->second >= g_uiTextures.size())
		return VRHI_INVALID_HANDLE;
	return g_uiTextures[found->second].texture;
}

static bool VRHI_AppendSceneQuad(const glm::vec3 corners[4],
	const glm::vec4 &color, qhandle_t shader) {
	if (g_sceneDraws.size() >= VRHI_MAX_SCENE_DRAWS ||
		g_sceneVertices.size() > VRHI_MAX_SCENE_VERTICES - 4 ||
		g_sceneIndexes.size() > VRHI_MAX_SCENE_INDEXES - 6) return false;
	const uint32_t firstVertex = static_cast<uint32_t>(g_sceneVertices.size());
	const uint32_t firstIndex = static_cast<uint32_t>(g_sceneIndexes.size());
	const glm::vec2 uv[4] = { glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f),
		glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f) };
	for (int i = 0; i < 4; ++i) g_sceneVertices.push_back(VRHI_SceneVertex(corners[i], uv[i], color));
	const uint32_t indexes[6] = { 0, 1, 2, 0, 2, 3 };
	for (uint32_t index : indexes) g_sceneIndexes.push_back(firstVertex + index);
	return VRHI_AppendSceneDraw(shader, firstIndex, 6);
}

static int VRHI_MD3Frame(const VRHI_MD3Model &model, int frame,
	const refEntity_t &entity) {
	if (model.numFrames <= 0) return 0;
	if ((entity.renderfx & RF_WRAP_FRAMES) != 0) {
		int wrapped = frame % model.numFrames;
		if (wrapped < 0) wrapped += model.numFrames;
		return wrapped;
	}
	return glm::clamp(frame, 0, model.numFrames - 1);
}
static bool VRHI_AppendMD3Model(const refEntity_t &entity, const glm::vec4 &color) {
	const auto found = g_md3ModelByHandle.find(entity.hModel);
	if (found == g_md3ModelByHandle.end() || found->second >= g_md3Models.size()) return false;
	const VRHI_MD3Model &model = g_md3Models[found->second];
	if (!model.valid || model.numFrames <= 0) return false;
	const int frame = VRHI_MD3Frame(model, entity.frame, entity);
	const int oldFrame = VRHI_MD3Frame(model, entity.oldframe, entity);
	const float backlerp = glm::clamp(entity.backlerp, 0.0f, 1.0f);
	const glm::vec3 origin(entity.origin[0], entity.origin[1], entity.origin[2]);
	for (const VRHI_MD3Surface &surface : model.surfaces) {
		if (surface.numVerts <= 0 || surface.numFrames != model.numFrames ||
			surface.st.size() != static_cast<size_t>(surface.numVerts) ||
			surface.xyz.size() != static_cast<size_t>(surface.numVerts) * static_cast<size_t>(model.numFrames) ||
			surface.indexes.empty() || surface.indexes.size() % 3 != 0) continue;
		const size_t indexCount = surface.indexes.size();
		// Check every per-scene cap before appending anything so a full
		// vertex/index/draw table never strands partial surface geometry.
		if (g_sceneVertices.size() > VRHI_MAX_SCENE_VERTICES - static_cast<size_t>(surface.numVerts) ||
			indexCount > VRHI_MAX_SCENE_INDEXES - g_sceneIndexes.size() ||
			g_sceneDraws.size() >= VRHI_MAX_SCENE_DRAWS) continue;
		// Build the surface in scratch first: the defensive validation below
		// (positions/UVs and index range) is already guaranteed by the parser,
		// but failing here must never leave the scene buffers half-written.
		std::vector<VRHI_WorldVertex> scratchVertices;
		std::vector<uint32_t> scratchIndexes;
		scratchVertices.reserve(static_cast<size_t>(surface.numVerts));
		scratchIndexes.reserve(indexCount);
		bool valid = true;
		for (int vertex = 0; vertex < surface.numVerts; ++vertex) {
			const VRHI_MD3XYZ &current = surface.xyz[static_cast<size_t>(frame) * static_cast<size_t>(surface.numVerts) + static_cast<size_t>(vertex)];
			const VRHI_MD3XYZ &old = surface.xyz[static_cast<size_t>(oldFrame) * static_cast<size_t>(surface.numVerts) + static_cast<size_t>(vertex)];
			const glm::vec3 currentPosition(current.x * static_cast<float>(MD3_XYZ_SCALE), current.y * static_cast<float>(MD3_XYZ_SCALE), current.z * static_cast<float>(MD3_XYZ_SCALE));
			const glm::vec3 oldPosition(old.x * static_cast<float>(MD3_XYZ_SCALE), old.y * static_cast<float>(MD3_XYZ_SCALE), old.z * static_cast<float>(MD3_XYZ_SCALE));
			const glm::vec3 local = currentPosition * (1.0f - backlerp) + oldPosition * backlerp;
			// Expand the basis explicitly at the API seam; this is the same
			// column-vector convention used by the Quake renderers.
			const glm::vec3 position = origin + glm::vec3(
				entity.axis[0][0] * local.x + entity.axis[1][0] * local.y + entity.axis[2][0] * local.z,
				entity.axis[0][1] * local.x + entity.axis[1][1] * local.y + entity.axis[2][1] * local.z,
				entity.axis[0][2] * local.x + entity.axis[1][2] * local.y + entity.axis[2][2] * local.z);
			const glm::vec2 uv = surface.st[static_cast<size_t>(vertex)];
			if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
				!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
				valid = false;
				break;
			}
			scratchVertices.push_back(VRHI_SceneVertex(position, uv, color));
		}
		if (!valid) continue;
		const uint32_t baseVertex = static_cast<uint32_t>(g_sceneVertices.size());
		for (uint32_t index : surface.indexes) {
			if (index >= static_cast<uint32_t>(surface.numVerts)) {
				valid = false;
				break;
			}
			scratchIndexes.push_back(index);
		}
		if (!valid) continue;
		const uint32_t firstIndex = static_cast<uint32_t>(g_sceneIndexes.size());
		g_sceneVertices.insert(g_sceneVertices.end(), scratchVertices.begin(), scratchVertices.end());
		for (uint32_t index : scratchIndexes) g_sceneIndexes.push_back(baseVertex + index);
		// Shader precedence mirrors the GL MD3 path: entity.customShader wins
		// for every surface, then the customSkin's surface-name override, then
		// the surface's embedded shader. skinNum is unsupported (VRHI retains
		// only the first embedded shader per surface) and is documented as such;
		// a skin entry whose shader could not resolve to a bounded direct image
		// keeps the safe solid fallback through the shared scene texture path.
		qhandle_t shader = entity.customShader;
		if (shader == 0 && entity.customSkin > 0) {
			shader = VRHI_SkinSurfaceShader(entity.customSkin, surface.name);
		}
		if (shader == 0) shader = surface.shader;
		// Every failure mode of AppendSceneDraw was pre-checked above; roll
		// back anyway so the surface stays atomic even if it ever changes.
		if (!VRHI_AppendSceneDraw(shader, firstIndex, indexCount)) {
			g_sceneVertices.resize(g_sceneVertices.size() - scratchVertices.size());
			g_sceneIndexes.resize(g_sceneIndexes.size() - scratchIndexes.size());
			continue;
		}
		++g_sceneModelDraws;
	}
	return true;
}

static bool VRHI_AppendInlineBSPModel(const refEntity_t &entity, const glm::vec4 &color) {
	const auto found = g_inlineBSPModelByHandle.find(entity.hModel);
	if (found == g_inlineBSPModelByHandle.end() || found->second >= g_inlineBSPModels.size()) return false;
	const VRHI_InlineBSPModel &model = g_inlineBSPModels[found->second];
	if (!model.valid || model.vertices.empty() || model.indexes.empty()) return false;
	const glm::vec3 origin(entity.origin[0], entity.origin[1], entity.origin[2]);
	for (const VRHI_WorldBatch &batch : model.batches) {
		if (batch.indexCount == 0 || batch.firstIndex > model.indexes.size() ||
			batch.indexCount > model.indexes.size() - batch.firstIndex ||
			batch.firstIndex > UINT32_MAX || g_sceneDraws.size() >= VRHI_MAX_SCENE_DRAWS) continue;
		std::vector<VRHI_WorldVertex> scratchVertices;
		std::vector<uint32_t> scratchIndexes;
		try {
			scratchVertices.reserve(std::min(static_cast<size_t>(batch.indexCount), model.vertices.size()));
			scratchIndexes.reserve(batch.indexCount);
			std::unordered_map<uint32_t, uint32_t> vertexMap;
			vertexMap.reserve(std::min(static_cast<size_t>(batch.indexCount), model.vertices.size()));
			bool lightmapped = false;
			for (size_t i = batch.firstIndex; i < static_cast<size_t>(batch.firstIndex) + batch.indexCount; ++i) {
				const uint32_t index = model.indexes[i];
				if (index >= model.vertices.size()) { scratchIndexes.clear(); break; }
				auto mapped = vertexMap.find(index);
				uint32_t sceneIndex = 0;
				if (mapped == vertexMap.end()) {
					const VRHI_WorldVertex &localVertex = model.vertices[index];
					const glm::vec3 &p = localVertex.position;
					const glm::vec3 position = origin + glm::vec3(
						entity.axis[0][0] * p.x + entity.axis[1][0] * p.y + entity.axis[2][0] * p.z,
						entity.axis[0][1] * p.x + entity.axis[1][1] * p.y + entity.axis[2][1] * p.z,
						entity.axis[0][2] * p.x + entity.axis[1][2] * p.y + entity.axis[2][2] * p.z);
					if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
						scratchIndexes.clear();
						break;
					}
					sceneIndex = static_cast<uint32_t>(scratchVertices.size());
					vertexMap.emplace(index, sceneIndex);
					lightmapped = lightmapped || localVertex.lightmapLayer >= -0.5f;
					// Carry the model-local lightmap UVs and array layer into the
					// scene vertex: the world lightmap atlas is shared with the
					// static BSP surfaces, and the pixel shaders gate on
					// lightmapLayer (< -0.5 means no lightmap for this vertex).
					VRHI_WorldVertex sceneVertex = VRHI_SceneVertex(position,
						localVertex.diffuse, color);
					sceneVertex.lightmap = localVertex.lightmap;
					sceneVertex.lightmapLayer = localVertex.lightmapLayer;
					scratchVertices.push_back(sceneVertex);
				} else sceneIndex = mapped->second;
				scratchIndexes.push_back(sceneIndex);
			}
			if (scratchIndexes.size() != batch.indexCount ||
				scratchVertices.size() > VRHI_MAX_SCENE_VERTICES ||
				scratchIndexes.size() > VRHI_MAX_SCENE_INDEXES ||
				g_sceneVertices.size() > VRHI_MAX_SCENE_VERTICES - scratchVertices.size() ||
				g_sceneIndexes.size() > VRHI_MAX_SCENE_INDEXES - scratchIndexes.size()) continue;
			const uint32_t baseVertex = static_cast<uint32_t>(g_sceneVertices.size());
			const uint32_t firstIndex = static_cast<uint32_t>(g_sceneIndexes.size());
			g_sceneVertices.insert(g_sceneVertices.end(), scratchVertices.begin(), scratchVertices.end());
			for (uint32_t index : scratchIndexes) g_sceneIndexes.push_back(baseVertex + index);
			if (!VRHI_AppendSceneDraw(entity.customShader, firstIndex, scratchIndexes.size(),
				batch.diffuseImage, lightmapped)) {
				g_sceneVertices.resize(g_sceneVertices.size() - scratchVertices.size());
				g_sceneIndexes.resize(g_sceneIndexes.size() - scratchIndexes.size());
				continue;
			}
			++g_sceneModelDraws;
		} catch (...) {
			// A malformed or exhausted transient allocation drops only this batch.
			continue;
		}
	}
	return true;
}

static bool VRHI_BuildSceneGeometry(const refdef_t *fd) {
	VRHI_ReserveSceneStorage();
	g_sceneVertices.clear();
	g_sceneIndexes.clear();
	g_sceneDraws.clear();
	g_sceneModelDraws = 0;
	if (fd == nullptr || !VRHI_FiniteVec3(fd->vieworg) ||
		!VRHI_FiniteVec3(fd->viewaxis[0]) || !VRHI_FiniteVec3(fd->viewaxis[1]) ||
		!VRHI_FiniteVec3(fd->viewaxis[2])) return false;
	const glm::vec3 viewForward(fd->viewaxis[0][0], fd->viewaxis[0][1], fd->viewaxis[0][2]);
	const glm::vec3 viewRight(fd->viewaxis[1][0], fd->viewaxis[1][1], fd->viewaxis[1][2]);
	const glm::vec3 viewUp(fd->viewaxis[2][0], fd->viewaxis[2][1], fd->viewaxis[2][2]);
	for (const VRHI_SceneEntity &submission : g_sceneEntities) {
		const refEntity_t &entity = submission.entity;
		const qhandle_t shader = entity.customShader;
		glm::vec4 color(entity.shaderRGBA[0] / 255.0f, entity.shaderRGBA[1] / 255.0f,
			entity.shaderRGBA[2] / 255.0f, entity.shaderRGBA[3] / 255.0f);
		if (color == glm::vec4(0.0f)) color = glm::vec4(1.0f);
		if (entity.reType == RT_MODEL) {
			if (g_md3ModelByHandle.find(entity.hModel) != g_md3ModelByHandle.end())
				VRHI_AppendMD3Model(entity, color);
			else
				VRHI_AppendInlineBSPModel(entity, color);
		} else if (entity.reType == RT_SPRITE) {
			if (!(entity.radius > 0.0f) || !std::isfinite(entity.radius)) continue;
			const float angle = entity.rotation * 3.14159265358979323846f / 180.0f;
			const float c = std::cos(angle), s = std::sin(angle);
			const glm::vec3 left = (c * viewRight - s * viewUp) * entity.radius;
			const glm::vec3 up = (c * viewUp + s * viewRight) * entity.radius;
			const glm::vec3 center(entity.origin[0], entity.origin[1], entity.origin[2]);
			const glm::vec3 corners[4] = { center - left - up, center + left - up,
				center + left + up, center - left + up };
			VRHI_AppendSceneQuad(corners, color, shader);
		} else if (VRHI_BeamFallbackSupported(static_cast<int>(entity.reType))) {
			// Bounded camera-facing beam-quad fallback shared by RT_BEAM,
			// RT_LIGHTNING, RT_RAIL_CORE, and RT_RAIL_RINGS. This is an
			// approximation: one flat view-facing quad along origin..oldorigin
			// with a fixed bounded per-type width, customShader texture, and
			// entity color/dynamic-light modulation. It is NOT full rail ring
			// geometry or lightning shader-stage parity.
			const glm::vec3 start(entity.origin[0], entity.origin[1], entity.origin[2]);
			const glm::vec3 end(entity.oldorigin[0], entity.oldorigin[1], entity.oldorigin[2]);
			const glm::vec3 direction = end - start;
			const float lengthSquared = glm::dot(direction, direction);
			if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f) continue;
			glm::vec3 side = glm::cross(direction, viewForward);
			float sideLength = glm::dot(side, side);
			if (!std::isfinite(sideLength) || sideLength <= 1.0e-8f) {
				side = glm::cross(direction, viewRight);
				sideLength = glm::dot(side, side);
			}
			if (sideLength <= 1.0e-8f || !std::isfinite(sideLength)) continue;
			// entity.frame is an int (finite by construction) and every width
			// is bounded by VRHI_BeamFallbackWidth.
			const float width = VRHI_BeamFallbackWidth(
				static_cast<int>(entity.reType), entity.frame);
			side *= width / std::sqrt(sideLength);
			const glm::vec3 corners[4] = { start - side, end - side, end + side, start + side };
			VRHI_AppendSceneQuad(corners, color, shader);
		}
	}
	for (const VRHI_ScenePoly &poly : g_scenePolys) {
		if (poly.numVerts < 3 || poly.firstVertex > g_scenePolyVerts.size() ||
			poly.numVerts > g_scenePolyVerts.size() - poly.firstVertex) continue;
		if (g_sceneVertices.size() > VRHI_MAX_SCENE_VERTICES - poly.numVerts ||
			poly.numVerts - 2 > (VRHI_MAX_SCENE_INDEXES - g_sceneIndexes.size()) / 3 ||
			g_sceneDraws.size() >= VRHI_MAX_SCENE_DRAWS) continue;
		const uint32_t firstVertex = static_cast<uint32_t>(g_sceneVertices.size());
		const uint32_t firstIndex = static_cast<uint32_t>(g_sceneIndexes.size());
		for (uint32_t i = 0; i < poly.numVerts; ++i) {
			const polyVert_t &source = g_scenePolyVerts[poly.firstVertex + i];
			const glm::vec4 color(source.modulate[0] / 255.0f, source.modulate[1] / 255.0f,
				source.modulate[2] / 255.0f, source.modulate[3] / 255.0f);
			g_sceneVertices.push_back(VRHI_SceneVertex(glm::vec3(source.xyz[0], source.xyz[1], source.xyz[2]),
				glm::vec2(source.st[0], source.st[1]), color));
		}
		for (uint32_t i = 1; i + 1 < poly.numVerts; ++i) {
			g_sceneIndexes.push_back(firstVertex);
			g_sceneIndexes.push_back(firstVertex + i);
			g_sceneIndexes.push_back(firstVertex + i + 1);
		}
		VRHI_AppendSceneDraw(poly.shader, firstIndex, (poly.numVerts - 2) * 3);
	}
	return !g_sceneIndexes.empty();
}

static bool VRHI_EnsureSceneBuffers(void) {
	if (!g_deviceInitialized || g_sceneVertices.empty() || g_sceneIndexes.empty()) return false;
	if (g_sceneVertexBuffer == VRHI_INVALID_HANDLE) g_sceneVertexBuffer = vhAllocBuffer();
	if (g_sceneIndexBuffer == VRHI_INVALID_HANDLE) g_sceneIndexBuffer = vhAllocBuffer();
	if (g_sceneVertexBuffer == VRHI_INVALID_HANDLE || g_sceneIndexBuffer == VRHI_INVALID_HANDLE) return false;
	if (!g_sceneBuffersCreated) {
		const int32_t errorsBefore = g_vhErrorCounter.load(std::memory_order_relaxed);
		vhCreateVertexBuffer(g_sceneVertexBuffer, "VRHI_SceneVertices", nullptr,
			"float3 float2 float2 float float4", VRHI_MAX_SCENE_VERTICES);
		vhCreateIndexBuffer(g_sceneIndexBuffer, "VRHI_SceneIndexes", nullptr,
			VRHI_MAX_SCENE_INDEXES, VRHI_BUFFER_INDEX32);
		vhFinish();
		if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
			VRHI_Printf(PRINT_WARNING, "renderer_vrhi: scene buffer creation failed\n");
			vhDestroyBuffer(g_sceneVertexBuffer);
			vhDestroyBuffer(g_sceneIndexBuffer);
			g_sceneVertexBuffer = g_sceneIndexBuffer = VRHI_INVALID_HANDLE;
			return false;
		}
		g_sceneBuffersCreated = true;
	}
	vhMem *vertices = new vhMem(g_sceneVertices.size() * sizeof(VRHI_WorldVertex));
	std::memcpy(vertices->data(), g_sceneVertices.data(), vertices->size());
	vhMem *indexes = new vhMem(g_sceneIndexes.size() * sizeof(uint32_t));
	std::memcpy(indexes->data(), g_sceneIndexes.data(), indexes->size());
	vhUpdateVertexBuffer(g_sceneVertexBuffer, vertices, 0, g_sceneVertices.size());
	vhUpdateIndexBuffer(g_sceneIndexBuffer, indexes, 0, g_sceneIndexes.size());
	return true;
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

// Recomputes the static world vertex color attribute from the submitted
// per-scene dynamic lights. Only the color field changes; positions, UVs,
// lightmap layer, and the index buffer are never touched. The persistent
// bounded scratch is sized to the world vertex count once per map, and the
// update is enqueued before the world draws so command ordering makes it
// visible to the same frame. Callers skip this entirely when no lights were
// submitted, keeping the no-light path a strict no-op with zero VRHI work.
static void VRHI_UpdateWorldVertexLighting(void) {
	if (!g_worldLoaded || g_worldVertices.empty() ||
		g_worldVertexBuffer == VRHI_INVALID_HANDLE) return;
	if (g_worldLightedVertices.size() != g_worldVertices.size()) {
		g_worldLightedVertices.resize(g_worldVertices.size());
	}
	for (size_t i = 0; i < g_worldVertices.size(); ++i) {
		g_worldLightedVertices[i] = g_worldVertices[i];
		g_worldLightedVertices[i].color =
			glm::vec4(VRHI_DLightModulation(g_worldVertices[i].position), 1.0f);
	}
	vhMem *vertices = new vhMem(g_worldLightedVertices.size() * sizeof(VRHI_WorldVertex));
	std::memcpy(vertices->data(), g_worldLightedVertices.data(), vertices->size());
	vhUpdateVertexBuffer(g_worldVertexBuffer, vertices, 0, g_worldLightedVertices.size());
}

static void VRHI_RenderScene(const refdef_t *fd) {
	if (fd == nullptr || (fd->rdflags & RDF_HYPERSPACE) != 0 ||
		!g_deviceInitialized || !g_frameBackbufferReady ||
		!g_worldShaderInitialized || g_worldDepthTexture == VRHI_INVALID_HANDLE ||
		fd->width <= 0 || fd->height <= 0 || !std::isfinite(fd->fov_x) ||
		!std::isfinite(fd->fov_y) || fd->fov_x <= 0.0f || fd->fov_y <= 0.0f ||
		!VRHI_FiniteVec3(fd->vieworg) || !VRHI_FiniteVec3(fd->viewaxis[0]) ||
		!VRHI_FiniteVec3(fd->viewaxis[1]) || !VRHI_FiniteVec3(fd->viewaxis[2])) return;
	if (!VRHI_CreateWorldDepth(g_frameViewportWidth, g_frameViewportHeight)) return;
	// Resolve the camera leaf/cluster and stamp the visible surface batches.
	// A failure to cull (no/malformed visibility, camera outside the tree)
	// falls back to drawing every batch, preserving the previous output.
	const glm::vec3 vieworg(fd->vieworg[0], fd->vieworg[1], fd->vieworg[2]);
	const bool cullActive = g_worldLoaded && g_worldVertexBuffer != VRHI_INVALID_HANDLE &&
		g_worldIndexBuffer != VRHI_INVALID_HANDLE && VRHI_MarkVisibleWorldBatches(vieworg);
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
	// Dynamic-light diagnostics: report the submitted per-scene light count
	// whenever it is non-zero so developer runs can prove lights reached the
	// renderer and were capped at MAX_DLIGHTS.
	if (!g_sceneLights.empty()) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: dynamic lights=%zu/%zu (bounded vertex modulation, add cap %.2f)\n",
			g_sceneLights.size(), static_cast<size_t>(VRHI_MAX_SCENE_LIGHTS),
			static_cast<double>(VRHI_DLIGHT_ADD_CAP));
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
		.SetTextures({})
		.SetSamplers({})
		.DirtyAll();
	if (g_worldLoaded && g_worldVertexBuffer != VRHI_INVALID_HANDLE &&
		g_worldIndexBuffer != VRHI_INVALID_HANDLE) {
		g_worldState.SetVertexBuffer(g_worldVertexBuffer, 0, 0, 0,
			static_cast<uint32_t>(g_worldVertices.size()))
			.SetIndexBuffer(g_worldIndexBuffer, 0, 0,
				static_cast<uint32_t>(g_worldIndexes.size()));
	}
	const vhState worldBaseState = g_worldState;
	g_worldDrawErrorBaseline = g_vhErrorCounter.load(std::memory_order_relaxed);
	// Per-scene dynamic lights modulate the static world vertex color
	// attribute in place before the static world draws. With no lights this
	// is a strict no-op; with lights, the enqueued update shares the world
	// draw error accounting below so failures surface in EndFrame's report.
	if (!g_sceneLights.empty() && g_worldVertexBuffer != VRHI_INVALID_HANDLE) {
		VRHI_UpdateWorldVertexLighting();
	}
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
	if (VRHI_BuildSceneGeometry(fd) && VRHI_EnsureSceneBuffers()) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: scene submissions: entities=%zu polys=%zu modelDraws=%zu draws=%zu vertices=%zu indexes=%zu lights=%zu\n",
			g_sceneEntities.size(), g_scenePolys.size(), g_sceneModelDraws, g_sceneDraws.size(),
			g_sceneVertices.size(), g_sceneIndexes.size(), g_sceneLights.size());
		for (const VRHI_SceneDraw &draw : g_sceneDraws) {
			const vhTexture texture = VRHI_SceneTexture(draw);
			const bool textured = texture != VRHI_INVALID_HANDLE &&
				g_worldDiffusePixelShader != VRHI_INVALID_HANDLE;
			const bool lightmapped = draw.lightmapped && g_worldLightmapTexture != VRHI_INVALID_HANDLE &&
				g_worldLightmapPixelShader != VRHI_INVALID_HANDLE;
			g_worldState = worldBaseState;
			g_worldState.SetProgram(textured ? g_worldDiffuseProgram :
				(lightmapped ? g_worldLightmapProgram : g_worldSolidProgram))
				.SetStateFlags(VRHI_STATE_WRITE_RGB | VRHI_STATE_WRITE_A | VRHI_STATE_WRITE_Z |
					VRHI_STATE_DEPTH_TEST_ENABLE | VRHI_STATE_DEPTH_TEST_LESS |
					VRHI_STATE_CULL_NONE | VRHI_STATE_PT_TRIANGLES | VRHI_STATE_BLEND_ALPHA)
				.SetVertexBuffer(g_sceneVertexBuffer, 0, 0, 0,
					static_cast<uint32_t>(g_sceneVertices.size()))
				.SetIndexBuffer(g_sceneIndexBuffer, 0, 0,
					static_cast<uint32_t>(g_sceneIndexes.size()))
				.SetTextures({}).SetSamplers({});
			if (textured) {
				g_worldState.SetTexture(0, { "u_diffuse", 0, texture })
					.SetSampler(0, { "u_diffuseSampler", 0,
						VRHI_SAMPLER_MIN_LINEAR | VRHI_SAMPLER_MAG_LINEAR |
						VRHI_SAMPLER_MIP_NONE | VRHI_SAMPLER_UVW_WRAP });
				// The shared diffuse shader declares its optional lightmap
				// resources even for scene/model draws. Bind the map lightmap
				// when available so model images never leave a declared slot
				// unbound; the no-world preview retains the solid fallback
				// behavior if no lightmap resource exists.
				if (g_worldLightmapTexture != VRHI_INVALID_HANDLE) {
					g_worldState.SetTexture(1, { "u_lightmap", 1, g_worldLightmapTexture })
						.SetSampler(1, { "u_lightmapSampler", 1,
							VRHI_SAMPLER_MIN_LINEAR | VRHI_SAMPLER_MAG_LINEAR |
							VRHI_SAMPLER_MIP_NONE | VRHI_SAMPLER_UVW_CLAMP });
				}
			} else if (lightmapped) {
				g_worldState.SetTexture(0, { "u_lightmap", 0, g_worldLightmapTexture })
					.SetSampler(0, { "u_lightmapSampler", 0,
						VRHI_SAMPLER_MIN_LINEAR | VRHI_SAMPLER_MAG_LINEAR |
						VRHI_SAMPLER_MIP_NONE | VRHI_SAMPLER_UVW_CLAMP });
			}
			if (vhSetState(g_worldStateId, g_worldState)) {
				if (!submitted) vhClear(g_worldStateId, VRHI_CLEAR_DEPTH);
				vhDrawIndexed(g_worldStateId, draw.indexCount, 1, draw.firstIndex);
				submitted = true;
			} else {
				VRHI_Printf(PRINT_WARNING,
					"renderer_vrhi: scene draw vhSetState failed (first=%u indexes=%u textured=%s)\n",
					draw.firstIndex, draw.indexCount, textured ? "yes" : "no");
			}
		}
	}
	g_worldDrawSubmitted = submitted;
	// RenderScene is also used for world-less scene frames (menu/model previews),
	// so only report a skipped draw when a loaded world produced no output.
	if (!submitted && g_worldLoaded) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: world/scene draw skipped (world vertices=%zu indexes=%zu scene submissions=%zu)\n",
			g_worldVertices.size(), g_worldIndexes.size(), g_sceneDraws.size());
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
static void VRHI_DrawUIRect(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, vhTexture texture) {
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
	const bool validUV = std::isfinite(s1) && std::isfinite(t1) &&
		std::isfinite(s2) && std::isfinite(t2);
	const bool textured = validUV && !g_uiTexturedProgram.empty() &&
		texture != VRHI_INVALID_HANDLE;
	const glm::vec4 uv(s1, t1, s2, t2);
	g_uiState.SetProgram(textured ? g_uiTexturedProgram : g_uiProgram)
		.SetUniform(0, { "ui_rect", { rect } })
		.SetUniform(1, { "ui_uv", { uv } })
		.SetUniform(2, { "ui_color", { g_uiColor } });
	g_uiState.SetTextures({}).SetSamplers({});
	if (textured) {
		g_uiState.SetTexture(0, { "ui_texture", 0, texture })
			.SetSampler(0, { "ui_sampler", 0,
				VRHI_SAMPLER_MIN_LINEAR | VRHI_SAMPLER_MAG_LINEAR |
				VRHI_SAMPLER_MIP_NONE | VRHI_SAMPLER_UVW_CLAMP });
	}
	if (!g_uiDrawSubmitted) {
		g_uiDrawErrorBaseline = g_vhErrorCounter.load(std::memory_order_relaxed);
	}
	if (vhSetState(g_uiStateId, g_uiState)) {
		vhDraw(g_uiStateId, 6);
		g_uiDrawSubmitted = true;
	} else {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: UI draw vhSetState failed (textured=%s)\n",
			textured ? "yes" : "no");
	}
}

static void VRHI_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader) {
	// Missing/unsupported handles keep the existing solid-color fallback
	// (texture == VRHI_INVALID_HANDLE), preserving DrawStretchPic semantics.
	vhTexture texture = VRHI_INVALID_HANDLE;
	const std::unordered_map<qhandle_t, size_t>::const_iterator found =
		g_uiTextureByHandle.find(shader);
	if (found != g_uiTextureByHandle.end() &&
		found->second < g_uiTextures.size()) {
		texture = g_uiTextures[found->second].texture;
	}
	VRHI_DrawUIRect(x, y, w, h, s1, t1, s2, t2, texture);
}

// Uploads one RGBA cinematic frame (cols x rows x 4 bytes) to the retained
// GPU texture of a bounded client slot. The frame pointer is copied into the
// upload buffer and never retained. Mirrors the GL2 semantics: the texture is
// (re)created whenever the slot is missing or its dimensions change, and an
// existing matching texture is updated only when dirty is true.
static void VRHI_UploadCinematicFrame(int cols, int rows, const byte *data,
	int client, qboolean dirty) {
	if (client < 0 || client >= VRHI_MAX_CINEMATIC_CLIENTS) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: cinematic client %d outside the %d-slot cap; dropped\n",
			client, VRHI_MAX_CINEMATIC_CLIENTS);
		return;
	}
	if (data == nullptr || cols <= 0 || rows <= 0) {
		return;
	}
	if (cols > VRHI_MAX_CINEMATIC_DIMENSION ||
		rows > VRHI_MAX_CINEMATIC_DIMENSION) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: cinematic frame %dx%d exceeds the %dx%d dimension cap; dropped\n",
			cols, rows, VRHI_MAX_CINEMATIC_DIMENSION,
			VRHI_MAX_CINEMATIC_DIMENSION);
		return;
	}
	const uint64_t frameBytes =
		static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows) * 4u;
	if (frameBytes > VRHI_MAX_CINEMATIC_FRAME_BYTES) {
		VRHI_Printf(PRINT_DEVELOPER,
			"renderer_vrhi: cinematic frame %dx%d (%llu RGBA bytes) exceeds the %zu-byte cap; dropped\n",
			cols, rows, static_cast<unsigned long long>(frameBytes),
			VRHI_MAX_CINEMATIC_FRAME_BYTES);
		return;
	}
	if (!g_deviceInitialized) {
		return;
	}

	VRHI_CinematicTexture &slot = g_cinematicTextures[static_cast<size_t>(client)];
	if (slot.texture == VRHI_INVALID_HANDLE || slot.width != cols ||
		slot.height != rows) {
		// Create (or recreate at a new size) the slot texture with this frame.
		if (slot.texture != VRHI_INVALID_HANDLE) {
			vhDestroyTexture(slot.texture);
			vhFinish();
			slot.texture = VRHI_INVALID_HANDLE;
		}
		slot.texture = vhAllocTexture();
		if (slot.texture == VRHI_INVALID_HANDLE) {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: cinematic client %d texture allocation failed\n",
				client);
			return;
		}
		vhMem *mem = new vhMem(static_cast<size_t>(frameBytes));
		std::memcpy(mem->data(), data, mem->size());
		const int32_t errorsBefore =
			g_vhErrorCounter.load(std::memory_order_relaxed);
		vhCreateTexture2D(slot.texture, "VRHI_Cinematic",
			glm::ivec2(cols, rows), 1, nvrhi::Format::RGBA8_UNORM,
			VRHI_TEXTURE_NONE | VRHI_SAMPLER_NONE, mem);
		vhFinish();
		if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
			VRHI_Printf(PRINT_WARNING,
				"renderer_vrhi: cinematic client %d upload failed; slot disabled\n",
				client);
			vhDestroyTexture(slot.texture);
			vhFinish();
			slot.texture = VRHI_INVALID_HANDLE;
			slot.width = 0;
			slot.height = 0;
			slot.uploaded = false;
			return;
		}
		slot.width = cols;
		slot.height = rows;
		slot.uploaded = true;
		VRHI_Printf(PRINT_ALL,
			"renderer_vrhi: cinematic client %d texture created (%dx%d RGBA)\n",
			client, cols, rows);
		return;
	}

	if (!dirty) {
		// dirty=false with a matching texture keeps the last uploaded frame;
		// a null data pointer is therefore safe here by construction.
		return;
	}
	vhMem *mem = new vhMem(static_cast<size_t>(frameBytes));
	std::memcpy(mem->data(), data, mem->size());
	const int32_t errorsBefore =
		g_vhErrorCounter.load(std::memory_order_relaxed);
	vhUpdateTexture(slot.texture, 0, 0, 1, 1, mem);
	vhFinish();
	if (g_vhErrorCounter.load(std::memory_order_relaxed) != errorsBefore) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: cinematic client %d frame update failed; slot disabled\n",
			client);
		vhDestroyTexture(slot.texture);
		vhFinish();
		slot.texture = VRHI_INVALID_HANDLE;
		slot.width = 0;
		slot.height = 0;
		slot.uploaded = false;
	}
}

static void VRHI_DrawStretchRaw(int x, int y, int w, int h, int cols,
	int rows, const byte *data, int client, qboolean dirty) {
	if (client < 0 || client >= VRHI_MAX_CINEMATIC_CLIENTS) {
		return;
	}
	VRHI_CinematicTexture &slot =
		g_cinematicTextures[static_cast<size_t>(client)];
	// DrawStretchRaw must be self-sufficient (the GL1 path also uploads): if
	// the slot has no usable texture or its dimensions no longer match the
	// current frame, (re)upload from this frame's data first.
	if (slot.texture == VRHI_INVALID_HANDLE || !slot.uploaded ||
		slot.width != cols || slot.height != rows || dirty) {
		if (data == nullptr) {
			return;
		}
		VRHI_UploadCinematicFrame(cols, rows, data, client, qtrue);
	}
	if (slot.texture == VRHI_INVALID_HANDLE || !slot.uploaded) {
		return;
	}
	// Without the textured UI program there is nothing to sample; do not draw
	// a solid-color box for video.
	if (g_uiTexturedProgram.empty()) {
		return;
	}
	VRHI_DrawUIRect(static_cast<float>(x), static_cast<float>(y),
		static_cast<float>(w), static_cast<float>(h),
		0.0f, 0.0f, 1.0f, 1.0f, slot.texture);
}
static void VRHI_UploadCinematic(int w, int h, int cols, int rows,
	const byte *data, int client, qboolean dirty) {
	// w/h are the on-screen dimensions and are ignored, matching the GL
	// renderers; cols/rows are the RGBA frame dimensions actually uploaded.
	(void)w;
	(void)h;
	VRHI_UploadCinematicFrame(cols, rows, data, client, dirty);
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
	if (tag != nullptr) std::memset(tag, 0, sizeof(*tag));
	const auto found = g_md3ModelByHandle.find(model);
	if (tag == nullptr || found == g_md3ModelByHandle.end() || found->second >= g_md3Models.size() ||
		tagName == nullptr || !std::isfinite(frac)) return 0;
	const VRHI_MD3Model &data = g_md3Models[found->second];
	if (!data.valid || data.numTags <= 0 || data.numFrames <= 0) return 0;
	const int start = glm::clamp(startFrame, 0, data.numFrames - 1);
	const int end = glm::clamp(endFrame, 0, data.numFrames - 1);
	const float blend = glm::clamp(frac, 0.0f, 1.0f);
	size_t tagNameLength = 0;
	if (!VRHI_MD3SafeName(tagName, &tagNameLength)) return 0;
	for (int index = 0; index < data.numTags; ++index) {
		const VRHI_MD3Tag &a = data.tags[static_cast<size_t>(start) * data.numTags + index];
		if (a.name.size() != tagNameLength || std::strncmp(a.name.c_str(), tagName, tagNameLength) != 0) continue;
		const VRHI_MD3Tag &b = data.tags[static_cast<size_t>(end) * data.numTags + index];
		tag->origin[0] = a.origin.x * (1.0f - blend) + b.origin.x * blend;
		tag->origin[1] = a.origin.y * (1.0f - blend) + b.origin.y * blend;
		tag->origin[2] = a.origin.z * (1.0f - blend) + b.origin.z * blend;
		for (int axis = 0; axis < 3; ++axis) for (int component = 0; component < 3; ++component)
			tag->axis[axis][component] = a.axis[axis][component] * (1.0f - blend) + b.axis[axis][component] * blend;
		return 1;
	}
	return 0;
}
static void VRHI_ModelBounds(qhandle_t model, vec3_t mins, vec3_t maxs) {
	if (mins != nullptr) std::memset(mins, 0, sizeof(vec3_t));
	if (maxs != nullptr) std::memset(maxs, 0, sizeof(vec3_t));
	const auto md3 = g_md3ModelByHandle.find(model);
	if (md3 != g_md3ModelByHandle.end() && md3->second < g_md3Models.size()) {
		const VRHI_MD3Model &data = g_md3Models[md3->second];
		if (!data.valid || data.frameMins.empty() || mins == nullptr || maxs == nullptr) return;
		for (int i = 0; i < 3; ++i) {
			mins[i] = data.frameMins[0][i];
			maxs[i] = data.frameMaxs[0][i];
		}
		return;
	}
	const auto inlineModel = g_inlineBSPModelByHandle.find(model);
	if (inlineModel == g_inlineBSPModelByHandle.end() ||
		inlineModel->second >= g_inlineBSPModels.size() || mins == nullptr || maxs == nullptr) return;
	const VRHI_InlineBSPModel &data = g_inlineBSPModels[inlineModel->second];
	if (!data.valid) return;
	for (int i = 0; i < 3; ++i) {
		mins[i] = data.mins[i];
		maxs[i] = data.maxs[i];
	}
}
static void VRHI_RegisterFont(const char *fontName, int pointSize,
	fontInfo_t *font) {
	// Fixed-cell fallback registration over the classic gfx/2d/bigchars
	// atlas (256x256, 16x16 grid of fixed 16x16-pixel cells). This is NOT
	// proportional or FreeType font parity: every glyph shares the single
	// atlas shader handle and identical fixed metrics; the bounded point
	// size only scales the fixed cell via glyphScale.
	if (font == nullptr) {
		return;
	}
	std::memset(font, 0, sizeof(*font));
	VRHI_CopyString(font->name, sizeof(font->name),
		fontName != nullptr ? fontName : "", "gfx/2d/bigchars");
	font->glyphScale = VRHI_FontGlyphScale(pointSize);

	const qhandle_t atlas = VRHI_RegisterShaderNoMip("gfx/2d/bigchars");
	if (atlas == 0) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: RegisterFont('%s', %d): bigchars atlas shader registration failed; empty font fallback\n",
			font->name, pointSize);
		return;
	}
	// Without a decoded atlas image the glyph handles would draw solid boxes.
	// Treat that as registration failure and keep the empty (invisible text)
	// fallback so the UI never renders garbage.
	if (g_uiTextureByHandle.find(atlas) == g_uiTextureByHandle.end()) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: RegisterFont('%s', %d): bigchars atlas image unavailable; empty font fallback\n",
			font->name, pointSize);
		return;
	}

	int populated = 0;
	for (int cp = 0; cp < 256; ++cp) {
		glyphInfo_t &glyph = font->glyphs[cp];
		float s = 0.0f, t = 0.0f, s2 = 0.0f, t2 = 0.0f;
		if (!VRHI_BigCharsCell(cp, &s, &t, &s2, &t2)) {
			continue; // unreachable for 0..255; keeps the glyph zeroed otherwise
		}
		glyph.height = VRHI_FONT_GLYPH_HEIGHT;
		glyph.top = VRHI_FONT_GLYPH_TOP;
		glyph.bottom = VRHI_FONT_GLYPH_BOTTOM;
		glyph.pitch = VRHI_FONT_GLYPH_PITCH;
		glyph.xSkip = VRHI_FONT_GLYPH_XSKIP;
		glyph.imageWidth = VRHI_FONT_GLYPH_IMAGE_WIDTH;
		glyph.imageHeight = VRHI_FONT_GLYPH_IMAGE_HEIGHT;
		glyph.s = s;
		glyph.t = t;
		glyph.s2 = s2;
		glyph.t2 = t2;
		glyph.glyph = atlas; // stable shared atlas handle for all codepoints
		VRHI_CopyString(glyph.shaderName, sizeof(glyph.shaderName),
			"gfx/2d/bigchars", "");
		++populated;
	}
	VRHI_Printf(PRINT_ALL,
		"renderer_vrhi: RegisterFont('%s', %d): bigchars fixed-cell fallback glyphs=%d scale=%g\n",
		font->name, VRHI_FontClampPointSize(pointSize), populated,
		static_cast<double>(font->glyphScale));
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
static void VRHI_TakeVideoFrame(int width, int height, byte *captureBuffer,
	byte *encodeBuffer, qboolean motionJpeg) {
	if (motionJpeg) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: MJPEG AVI capture is unsupported; no frame queued\n");
		return;
	}
	if (!g_deviceInitialized || width <= 0 || height <= 0 ||
		width > VRHI_MAX_VIDEO_DIMENSION || height > VRHI_MAX_VIDEO_DIMENSION ||
		encodeBuffer == nullptr) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: AVI video capture rejected: invalid %dx%d, NULL encode "
			"buffer, or renderer unavailable (cap=%d)\n", width, height,
			VRHI_MAX_VIDEO_DIMENSION);
		return;
	}
	const size_t w = static_cast<size_t>(width);
	const size_t h = static_cast<size_t>(height);
	const size_t captureBytes = w * 4 * h;
	const size_t aviPitch = ((w * 3) + (VRHI_AVI_LINE_PADDING - 1)) /
		VRHI_AVI_LINE_PADDING * VRHI_AVI_LINE_PADDING;
	if (captureBytes > VRHI_MAX_VIDEO_FRAME_BYTES ||
		h > (std::numeric_limits<size_t>::max)() / aviPitch ||
		aviPitch * h > VRHI_MAX_VIDEO_FRAME_BYTES ||
		aviPitch * h > static_cast<size_t>(std::numeric_limits<int>::max())) {
		VRHI_Printf(PRINT_WARNING,
			"renderer_vrhi: AVI video capture rejected: frame exceeds bounded caps\n");
		return;
	}
	g_videoCapture.pending = true;
	g_videoCapture.width = width;
	g_videoCapture.height = height;
	g_videoCapture.captureBuffer = captureBuffer;
	g_videoCapture.encodeBuffer = encodeBuffer;
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
		"renderer_vrhi: loaded (clear/present + bounded sprite/beam/poly scenes + direct-image UI + fixed-cell bigchars fonts + bounded RGBA cinematics + lightmapped/image PVS-culled BSP world + bounded .skin MD3 skins)\n");
	return &exports;
}
