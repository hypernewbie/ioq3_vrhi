# VRHI renderer slice

`renderer_vrhi` is an optional, dynamically loaded Windows renderer. It consumes
copied VRHI release/debug archives; the VRHI submodule is not added to the
ioquake3 CMake graph.

The current slice owns an SDL `SDL_WINDOW_VULKAN` window, initializes one VRHI
device and primary swapchain, clears the acquired backbuffer, and presents once
per engine frame. `Shutdown(qfalse)` flushes while retaining the device/window
for a video restart; `Shutdown(qtrue)` finishes and destroys VRHI, input, the
window, and SDL video in that order.

Model, skin, shader, image, world, scene, UI, font, cinematic, screenshot, and
video-capture resources are intentionally not implemented yet. Their complete
refexport callbacks are safe no-ops so the client, cgame, and UI cannot
dereference a null renderer callback. The registration callbacks (models,
skins, shaders) return stable nonzero engine-local handles with the same
name-to-handle semantics as the GL renderers, so engine code that treats
qhandle_t 0 as a load failure proceeds normally; only the name->handle mapping
is retained, never image, model, or world data. Resize/minimize failures are
reported as warnings and are not treated as fatal initialization errors.
