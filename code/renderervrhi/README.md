# VRHI renderer slice

`renderer_vrhi` is an optional, dynamically loaded Windows renderer. It consumes
copied VRHI release/debug archives; the VRHI submodule is not added to the
ioquake3 CMake graph.

The current slice owns an SDL `SDL_WINDOW_VULKAN` window, initializes one VRHI
device and primary swapchain, clears the acquired backbuffer, draws solid-color
UI rectangles through `DrawStretchPic`, and presents once per engine frame. This
UI path is a fallback only, not texture rendering parity.

The static BSP world path is deliberately scoped to planar and triangle-soup
surfaces. It preserves BSP lightmap UVs and indices, uploads raw 128x128 RGB
lightmap blocks as a VRHI texture array, and samples them with a fixed world
shader. A bounded diffuse subset decodes direct, uncompressed 24/32-bit TGA
files named by BSP shaders (the shader name itself, or its `.tga` suffix),
retains per-surface UVs, and issues texture batches. Missing or unsupported
files use the existing lightmap/solid path. TGA dimensions, image count, and
aggregate decoded memory are capped. Shader-script parsing, JPG/PNG, TGA RLE,
PK3 material stages, patches, visibility, entities, and corresponding renderer
parity are not implemented, so visual coverage remains limited and maps may
differ substantially from the GL renderers.

`Shutdown(qfalse)` flushes while retaining the device/window for a video restart;
`Shutdown(qtrue)` finishes and destroys VRHI, input, the window, and SDL video in
that order.

Model, skin, general shader/image registration, scene, textured UI, font,
cinematic, and video-capture resources are intentionally not implemented yet;
only the scoped static BSP geometry/lightmap/direct-TGA diffuse path above is
present. Every refexport callback is populated so the client, cgame, and UI
cannot dereference a null renderer callback: model, skin, and shader
registration returns stable nonzero engine-local handles with the same
name-to-handle semantics as the GL renderers, while the remaining unsupported
callbacks are safe no-ops. The renderer-owned `screenshot` command arms a
backbuffer capture ticket for the final clear/world/UI frame; EndFrame
synchronously reads the bound backbuffer and writes a validated 24-bit BGR TGA
when VRHI supports the readback. Resize/minimize failures are reported as
warnings and are not treated as fatal initialization errors.
