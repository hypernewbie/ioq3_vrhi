# VRHI renderer slice

`renderer_vrhi` is an optional, dynamically loaded Windows renderer. It consumes
copied VRHI release/debug archives; the VRHI submodule is not added to the
ioquake3 CMake graph.

The current slice owns an SDL `SDL_WINDOW_VULKAN` window, initializes one VRHI
device and primary swapchain, clears the acquired backbuffer, draws direct
uncompressed/RLE type 2/10 24/32-bit TGA UI textures through `DrawStretchPic`
(including s1/t1/s2/t2 UVs multiplied by `SetColor`), and presents once per
engine frame. Missing, malformed, unsupported, or non-TGA material names use a
solid-color fallback.

The static BSP world path is deliberately scoped to planar and triangle-soup
surfaces. It preserves BSP lightmap UVs and indices, uploads raw 128x128 RGB
lightmap blocks as a VRHI texture array, and samples them with a fixed world
shader. A bounded diffuse subset decodes direct, uncompressed (type 2) and
run-length encoded (type 10) 24/32-bit true-color TGA files named by BSP shaders
(the shader name itself, or its `.tga` suffix), retains per-surface UVs, and
issues texture batches. Missing or unsupported files use the existing
lightmap/solid path. The decoder validates packet bounds, pixel counts, run
lengths, dimensions, orientation, and decoded memory. TGA dimensions, image
count, and aggregate decoded memory are capped. Static world batches are
PVS-culled: BSP nodes/leafs/leafsurfaces/planes/visibility lumps are decoded
with little-endian safety into bounded CPU copies, the camera leaf is located
from `refdef.vieworg`, the visible cluster bitset is decoded, and only batches
reachable from visible leaves are drawn. The vertex/index buffers stay static;
culling only skips indexed ranges per batch. When visibility is absent or
malformed, or the camera leaf cannot be resolved, the renderer falls back to
drawing every batch. Per-frame visible cluster/batch/index counts are reported at
developer level, or every frame at `PRINT_ALL` with `r_vrhi_cullDebug 1`.
Shader-script parsing, JPG/PNG, PK3 material stages, area/door masking, patches,
entities, and corresponding renderer parity are not implemented, so visual
coverage remains limited and maps may differ substantially from the GL
renderers.

`Shutdown(qfalse)` flushes while retaining the device/window for a video restart;
`Shutdown(qtrue)` finishes and destroys VRHI, input, the window, and SDL video in
that order.

Model, skin, general shader-script/JPG/PNG/PK3 material stages, scene entities,
patches, fonts, cinematics, and video-capture resources are intentionally not
implemented. Shader registration only admits direct uncompressed/RLE type 2/10
24/32-bit TGA names (a bare shader name resolves to `.tga`, or an explicit
`.tga` suffix); all other material semantics remain unsupported and use the
solid UI fallback. Every refexport callback is populated so the client, cgame,
and UI cannot dereference a null renderer callback. Model, skin, and unsupported
material registrations retain stable name-to-handle mappings; eligible UI TGA
pixels are retained under bounded caps for video restart while their VRHI
textures are destroyed and re-uploaded at the next registration, and final
shutdown destroys both GPU and CPU resources. The renderer-owned `screenshot`
command arms a backbuffer capture ticket for the final clear/world/UI frame;
EndFrame synchronously reads the bound backbuffer and writes a validated 24-bit
BGR TGA when VRHI supports the readback. The diffuse TGA decoder lives in
`vrhi_tga_decode.h` and is exercised standalone by
`tests/vrhi_tga_decode_test.cpp` (any C++17 compiler; no engine or third-party
dependencies). Resize/minimize failures are reported as warnings and are not
treated as fatal initialization errors.
