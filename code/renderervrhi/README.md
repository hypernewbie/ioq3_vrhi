# VRHI renderer slice

`renderer_vrhi` is an optional, dynamically loaded Windows renderer. It consumes
copied VRHI release/debug archives; the VRHI submodule is not added to the
ioquake3 CMake graph.

The current slice owns an SDL `SDL_WINDOW_VULKAN` window, initializes one VRHI
device and primary swapchain, clears the acquired backbuffer, draws solid-color
UI rectangles through `DrawStretchPic`, and presents once per engine frame. This
UI path is a fallback only, not texture rendering parity.

The static BSP world path is deliberately scoped to planar and triangle-soup
surfaces. It is triangle-soup geometry only: patches, materials, lightmaps,
visibility, entities, and the corresponding renderer parity are not implemented,
so visual coverage and lighting remain limited and maps may differ substantially
from the GL renderers.
`Shutdown(qfalse)` flushes while retaining the device/window for a video restart;
`Shutdown(qtrue)` finishes and destroys VRHI, input, the window, and SDL video in
that order.

Model, skin, shader, image, scene, textured UI, font, cinematic, and
video-capture resources are intentionally not implemented yet; only the scoped
static BSP world geometry path above is present. Their complete refexport
callbacks are safe no-ops so the client, cgame, and UI cannot
dereference a null renderer callback. The renderer-owned `screenshot` command
arms a backbuffer capture ticket for the rendered clear/UI frame; EndFrame
synchronously reads the bound backbuffer and writes a validated 24-bit BGR TGA
when VRHI supports the readback. The registration callbacks (models, skins,
shaders) return stable nonzero engine-local handles with the same
name-to-handle semantics as the GL
renderers, so engine code that treats qhandle_t 0 as a load failure proceeds
normally; only the name->handle mapping is retained, never image, model, or
world data. Resize/minimize failures are reported as warnings and are not
treated as fatal initialization errors.
