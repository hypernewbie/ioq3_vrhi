# VRHI renderer slice

`renderer_vrhi` is an optional, dynamically loaded Windows renderer. It consumes
copied VRHI release/debug archives; the VRHI submodule is not added to the
ioquake3 CMake graph.

The current slice owns an SDL `SDL_WINDOW_VULKAN` window, initializes one VRHI
device and primary swapchain, clears the acquired backbuffer, draws bounded TGA, JPG/JPEG, and PNG UI textures through `DrawStretchPic`
(including s1/t1/s2/t2 UVs multiplied by `SetColor`), and presents once per
engine frame. Missing, malformed, unsupported, or oversized material names use
a solid-color fallback.

The static BSP world path supports planar, triangle-soup, and bounded quadratic
patch surfaces. Patch control grids must have odd dimensions of at least 3,
pass finite/range checks, and stay within explicit control, block, and aggregate
geometry caps; each overlapping 3x3 block is tessellated at four subdivisions
with nondegenerate triangles and retains interpolated diffuse/lightmap UVs.
Each patch is one batch mapped to its original BSP surface, so leafsurface PVS
culling remains intact. It preserves BSP lightmap UVs and indices, uploads raw
128x128 RGB lightmap blocks as a VRHI texture array, and samples them with a
fixed world shader. A bounded diffuse subset decodes direct, uncompressed (type 2)
and run-length encoded (type 10) 24/32-bit true-color TGA files plus JPG/JPEG and
8-bit RGB/RGBA/grayscale/gray-alpha/indexed PNG files named by BSP shaders (bare
names probe bounded supported extensions; explicit extensions are preserved),
retains per-surface UVs, and issues texture batches. Missing, corrupt,
unsupported, or oversized files use the existing lightmap/solid path. Image
dimensions, compressed/decompressed bytes, image count, and aggregate decoded
memory are capped; PNG signatures/chunks/CRCs, filters, zlib checks, and
non-interlaced 8-bit modes are validated, while JPG uses libjpeg setjmp
recovery and RGB conversion. Static world batches are
PVS-culled: BSP nodes/leafs/leafsurfaces/planes/visibility lumps are decoded
with little-endian safety into bounded CPU copies, the camera leaf is located
from `refdef.vieworg`, the visible cluster bitset is decoded, and only batches
reachable from visible leaves are drawn. The per-frame `refdef.areamask` is
honored during that traversal: a set bit closes the corresponding portal
area, so leaves whose decoded area bit is masked are skipped, while negative
or out-of-range area values are never masked and the absent/malformed-PVS
all-visible fallback is unchanged. The vertex/index buffers stay static;
culling only skips indexed ranges per batch. When visibility is absent or
malformed, or the camera leaf cannot be resolved, the renderer falls back to
drawing every batch. Per-frame visible cluster/batch/index counts are reported at
developer level, or every frame at `PRINT_ALL` with `r_vrhi_cullDebug 1`.
A bounded simple multi-stage shader-script material slice is supported for
static BSP maps: `scripts/*.shader` is scanned once per map and matching names
copy at most four direct-image stages (`map`/`clampmap`, TGA/JPG/JPEG/PNG) into a
cache. Stage 0 is opaque (no blend or exact `GL_ONE GL_ZERO`); later stages may
use only `blend`/source-alpha blending or `GL_ONE GL_ONE` additive blending.
The parser validates allowlisted identity/depth statements token-for-token and
rejects tcGen/tcMod, deform/fog/animMap/videoMap/normal/specular/portal/sky,
special-only maps, malformed statements, and stage/path/image caps. Static BSP
batches retain copied stage descriptors; stage 0 uses diffuse x lightmap and
overlays preserve image alpha without lightmap modulation. Inline BSP/model
scene draws intentionally retain only stage 0 to keep the bounded scene path
simple. This is not full Quake shader/material parity; unsupported scripts
fall back to the existing lightmap/solid path. The client `refdef.areamask`
door culling remains a separate mechanism and IS honored during PVS traversal.
Script file count, per-file/aggregate text, token, stage, candidate image, and
decoded image memory are bounded. The dependency-free parser lives in
`vrhi_shader_script.h` and is covered by `tests/vrhi_shader_script_test.cpp`.

Dynamic lights (`AddLightToScene`/`AddAdditiveLightToScene`) are stored as
validated per-scene point lights under the engine's strict `MAX_DLIGHTS` cap
(32) and reset on `ClearScene`, restart, and shutdown; non-finite origins,
colors, or intensity, non-positive intensity, and negative color channels are
rejected, and color channels above 1.0 are clamped. This is bounded vertex
modulation, NOT full Quake lightmap/shader parity: each light contributes
`clamp(1 - dist/radius, 0, 1)^2 * color` to a vertex and the final vertex
color is `base * (1 + clamp(add, 0, 1))`, so surfaces brighten with a
finite-distance falloff and never receive unbounded values. Before the static
world draws, the existing world vertex color attribute is recomputed from its
base white using this modulation through a persistent bounded scratch; only
the color field changes, the static geometry and index buffer are never
modified, and the update is enqueued before the draws so the same frame sees
it. The identical modulation multiplies generated sprite/beam/poly/MD3 scene
vertices before the dynamic upload. With no lights every path is a strict
no-op (no CPU work, no VRHI commands). `LightForPoint` answers consistently
from the submitted lights (byte-scale directed light plus a falloff-weighted
direction; zeroed ambient), returning `qfalse` with zeroed outputs when no
lights or a non-finite query point is supplied. There is no light grid, no
shadowing, and no directionality; additive and regular lights add the same
way, and per-frame submitted light counts are reported at developer level.
The dependency-free math lives in `vrhi_dlight.h` and is exercised standalone
by `tests/vrhi_dlight_test.cpp`.

`Shutdown(qfalse)` flushes while retaining the device/window for a video restart;
`Shutdown(qtrue)` finishes and destroys VRHI, input, the window, and SDL video in
that order.

RT_SPRITE entities plus AddPolyToScene triangle-fan batches are
retained in bounded CPU scene storage and rendered as camera-facing 3D geometry
after the static world. RT_BEAM, RT_LIGHTNING, RT_RAIL_CORE, and RT_RAIL_RINGS
are emitted through the same bounded camera-facing beam-quad fallback: one flat
view-facing quad spanning entity origin..oldorigin with a fixed bounded
per-type width (beam honors a bounded `frame` scale; lightning 8, rail core 6,
rail rings 16), textured with the entity customShader (solid fallback) and
modulated by entity color plus the bounded dynamic-light add. This is a
beam-quad approximation, NOT full rail ring geometry or lightning shader-stage
parity: no rings, no rotated rail-core passes, no shader stages, and no segment
animation; the GL renderers' r_railWidth/r_railCoreWidth cvars are not honored.
Registered direct image handles are reused when
available, with a solid fallback. Scene CPU submissions and renderer-owned
transient vertex/index buffers reset on ClearScene and are destroyed on restart
and shutdown. RT_MODEL supports bounded MD3 data and safe inline BSP names
(`*1`, `*2`, ...): inline planar/triangle-soup/patch surfaces retain local
positions, diffuse/lightmap UVs, lightmap layers, and shared BSP diffuse image
indices, then transform by refEntity origin/axis with entity color and dynamic
light modulation. Inline registration is refreshed after each world load so
handles registered before a load cannot retain stale model indices; ModelBounds
works for both MD3 and inline BSP models, while LerpTag remains false for inline.
Inline geometry has strict per-model and aggregate caps and malformed surfaces
are skipped safely. MD3 normal decoding/lighting and full material/shader
parity remain unsupported, as do portal and video-capture
resources (safe no-ops); bounded surface-name `.skin` overrides are supported
(next paragraph).

`RegisterSkin` parses bounded Quake `.skin` text into per-surface shader
overrides used by RT_MODEL MD3 draws, with the GL renderers' precedence:
`entity.customShader` wins for every surface, then the `customSkin`
surface-name override, then the surface's embedded shader (`skinNum` remains
unsupported). Tokenization mirrors the GL CommaParse skin loop: whitespace (any
char <= ' '), commas, and `//` line comments separate tokens, CRLF is plain
whitespace, bare `tag_` surface lines are skipped without consuming a shader
token, overlong tokens drop the whole malformed entry so the surface/shader
alternation stays aligned, and a surface whose shader is missing at end-of-text
is dropped. Surface names are lowercased for case-insensitive MD3 matching
while shader qpaths keep their case, and every shader path is re-validated as a
safe qpath before it reuses the bounded direct-image registration path
(unsupported scripts/materials keep the safe solid fallback). File text,
per-token, per-skin entry, aggregate entry, and skin count are strictly bounded
(1 MiB file cap, 64-char token cap, 256 entries per skin, 8192 aggregate
entries, 1024 skins; handle 0 stays reserved as "default skin"), and
missing/empty/malformed `.skin` files return qhandle 0 with a bounded failure
cache, exactly like the GL renderers. The dependency-free parser lives in
`vrhi_skin.h` and is exercised standalone by `tests/vrhi_skin_test.cpp`.

`RegisterFont` is a documented fixed-cell fallback, NOT proportional/FreeType
parity: it registers the classic `gfx/2d/bigchars` atlas (256x256, 16x16 grid
of fixed 16x16-pixel cells, mapped exactly like SCR_DrawSmallChar) through
`RegisterShaderNoMip`, populates all 256 `fontInfo_t` glyphs with a stable
shared atlas handle, fixed height/top/bottom/pitch/xSkip/image dimensions, and
16x16-cell UVs, and computes `glyphScale` from the point size (clamped to
[8,128], 48 points = 1:1). If the atlas shader or its decoded image is
unavailable the font stays an empty (invisible-text) fallback instead of
drawing solid boxes. Shader registration admits bounded TGA/JPG/JPEG/PNG names (bare
names probe `.tga`, `.jpg`, `.jpeg`, and `.png`; explicit supported extensions are not
rewritten); static BSP shader scripts use the same resolver for their bounded
four-stage material slice, while unsupported UI material semantics use the
solid UI fallback.

Cinematics (`UploadCinematic`/`DrawStretchRaw`) upload transient RGBA frames to
retained per-client VRHI textures under a strict small client-slot cap (8), a
2048-per-side dimension cap, and a 16 MiB per-frame byte cap; the caller's
frame pointer is copied for the upload and never retained, so no unbounded
frame data is kept. Inputs are validated (positive cols/rows, non-null RGBA
data, in-range client, caps) and `dirty=false` with a matching texture keeps
the last frame (null data is then safe by construction). `DrawStretchRaw` is
self-sufficient like the GL1 path: it (re)uploads when the slot is empty or the
frame dimensions changed, then draws a full-cell textured UI rectangle through
the same UI state/program and UV/color semantics as `DrawStretchPic`. Slots are
destroyed on `Shutdown(qfalse)` restart, `Shutdown(qtrue)` final teardown, and
reset on no-device failures. Raw AVI BI_RGB capture is supported: a bounded
one-frame-late request reads the final pre-present RGBA/BGRA backbuffer, fills
the optional caller capture buffer, and writes bottom-up padded BGR rows.
MJPEG capture remains explicitly unsupported and queues no frame; unsupported
framebuffer formats/readback failures fall back with a warning. Every refexport
callback is populated so the client, cgame, and UI cannot dereference a null
renderer callback. Model, skin, shader, and
unsupported material registrations retain stable name-to-handle mappings; eligible UI image
pixels are retained under bounded caps for video restart while their VRHI
textures are destroyed and re-uploaded at the next registration, and final
shutdown destroys both GPU and CPU resources. The renderer-owned `screenshot`
command arms a backbuffer capture ticket for the final clear/world/UI frame;
EndFrame synchronously reads the bound backbuffer and writes a validated 24-bit
BGR TGA when VRHI supports the readback. The dependency-free TGA decoder lives in `vrhi_tga_decode.h`; PNG and JPEG
are exposed by the focused `vrhi_image_decode.h` API and use puff/libjpeg
target-locally. The TGA decoder is exercised standalone by
`tests/vrhi_tga_decode_test.cpp` (any C++17 compiler; no engine or third-party
dependencies), the fixed-cell font UV/scale helpers are exercised
standalone by `tests/vrhi_font_test.cpp`, the bounded `.skin` text parser
is exercised standalone by `tests/vrhi_skin_test.cpp`, and the bounded
beam-quad fallback type/width helpers are exercised standalone by
`tests/vrhi_beam_test.cpp`. Resize/minimize failures are reported as warnings and are not
treated as fatal initialization errors.
