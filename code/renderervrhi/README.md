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
reachable from visible leaves are drawn. The vertex/index buffers stay static;
culling only skips indexed ranges per batch. When visibility is absent or
malformed, or the camera leaf cannot be resolved, the renderer falls back to
drawing every batch. Per-frame visible cluster/batch/index counts are reported at
developer level, or every frame at `PRINT_ALL` with `r_vrhi_cullDebug 1`.
A bounded first-stage shader-script diffuse lookup is supported for BSP maps:
`scripts/*.shader` is scanned once per map, matching BSP shader names and taking
the first non-special `map`/`clampmap` image stage into a copied cache. This
is not full Quake shader/material parity; deform, fog, blend, additional
stage/material semantics, PK3 material stages, area/door masking, and
corresponding renderer parity remain unsupported and fall back to the existing
lightmap/solid path. Script file count, per-file/aggregate text,
token, candidate image, and decoded image memory are bounded, so malformed or
oversized input is rejected safely.

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

RT_SPRITE and RT_BEAM entities plus AddPolyToScene triangle-fan batches are
retained in bounded CPU scene storage and rendered as camera-facing 3D geometry
after the static world. Registered direct image handles are reused when
available, with a solid fallback. Scene CPU submissions and renderer-owned
transient vertex/index buffers reset on ClearScene and are destroyed on restart
and shutdown. RT_MODEL (including MD3 parsing and inline BSP submodels), rail,
lightning, portal, fonts, cinematics, and video-capture resources remain
explicit safe no-ops; no fake model parity is attempted. Shader registration admits bounded TGA/JPG/JPEG/PNG names (bare
names probe `.tga`, `.jpg`, `.jpeg`, and `.png`; explicit supported extensions are not
rewritten); the BSP-only first-stage script lookup uses the same image resolver,
and all other material semantics remain unsupported and use the solid UI
fallback. Every refexport callback is populated so the client, cgame,
and UI cannot dereference a null renderer callback. Model, skin, and unsupported
material registrations retain stable name-to-handle mappings; eligible UI image
pixels are retained under bounded caps for video restart while their VRHI
textures are destroyed and re-uploaded at the next registration, and final
shutdown destroys both GPU and CPU resources. The renderer-owned `screenshot`
command arms a backbuffer capture ticket for the final clear/world/UI frame;
EndFrame synchronously reads the bound backbuffer and writes a validated 24-bit
BGR TGA when VRHI supports the readback. The dependency-free TGA decoder lives in `vrhi_tga_decode.h`; PNG and JPEG
are exposed by the focused `vrhi_image_decode.h` API and use puff/libjpeg
target-locally. The TGA decoder is exercised standalone by
`tests/vrhi_tga_decode_test.cpp` (any C++17 compiler; no engine or third-party
dependencies). Resize/minimize failures are reported as warnings and are not
treated as fatal initialization errors.
