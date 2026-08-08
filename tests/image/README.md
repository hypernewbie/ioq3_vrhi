# OpenArena image smoke tests

These tests are external to ioquake3. They use the pinned OpenArena assets and
run the existing renderer process in a fresh ignored home directory for every
scene and repetition.

The runner is deliberately safe for a desktop:

- SDL windows are hidden with Win32 `ShowWindow(SW_HIDE)` polling on Windows.
- Sound initialization and volume are disabled, and SDL uses its dummy audio driver.
- Every process has a timeout. A timeout kills the complete process tree.
- Engine stdout/stderr and screenshots are written below `temp/image-tests/`.
- No test image or runtime state is committed.

## Commands

List scenes:

```text
python tests/image/run_image_tests.py --list-scenes
```

Run the complete OpenGL 2 suite twice:

```text
python tests/image/run_image_tests.py \
  --engine build/llvm-clangcl-release/Release/ioquake3.exe \
  --renderer opengl2 --repeat 2 --timeout 90
```

Run the diagnostic OpenGL 1 suite:

```text
python tests/image/run_image_tests.py \
  --engine build/llvm-clangcl-release/Release/ioquake3.exe \
  --renderer opengl1 --repeat 2 --timeout 90
```

Run one scene or a subset manually:

```text
python tests/image/run_image_tests.py --scene demo088-test1
python tests/image/run_image_tests.py --scene map-oa-dm1 --scene map-oa-dm3
```

Run the hidden VRHI UI capture (renderer-owned final-present readback):

```text
python tests/image/run_image_tests.py \
  --engine build/verify-vrhi/Release/ioquake3.exe \
  --renderer vrhi --scene map-oa-dm1 --repeat 1 --timeout 90
```

VRHI map scenes are capture-only; add `cg_draw2D 1` through the scene's cvars
in an ad-hoc manifest to paint UI rectangles into the capture.

Run the dependency-free unit tests and the controlled fixture manifest:

```text
python -m unittest discover tests/image -p "test_*.py"
python tests/image/classify_fixtures.py
```

Classify a real capture without any GPU (prints JSON):

```text
python tests/image/frame_classify.py temp/image-tests/<run>/home/baseoa/screenshots/map-oa-dm1.tga
```

## Current scope

Captures come from the active renderer's own `screenshot` command. The demo
scene checks pinned demo playback and requires exact repeatability. Map scenes
start a local server, enable local cheats, move to a pinned spawn/view position,
and check static-world startup. Map captures are currently capture-only because
the stock client path has small timing-dependent scene differences; their raw
hashes and logs are retained for later calibration.

For the `vrhi` renderer the capture is the renderer-owned backbuffer readback
implemented in `code/renderervrhi` (see `code/renderervrhi/README.md`); the
runner inserts two `wait` commands between `screenshot` and `quit` so the
renderer's EndFrame services the capture ticket before shutdown (Cbuf_Execute
runs twice per engine frame, so one wait is not enough to guarantee a frame
boundary). VRHI runs therefore report
`authoritative_capture: true` in the report. GL runs remain a
plumbing/repeatability test, not a final-present or OpenGL-versus-VRHI
correctness oracle.

The `.dm_70` OpenArena demos are intentionally not in the manifest because this
ioquake3 build does not support protocol 70. Their maps are covered through
`+map` scenes instead.

## Frame classification and renderer-slice gating

`tests/image/frame_classify.py` is a pure-Python, GPU-free oracle that labels a
normalized RGB8 capture as one of:

- `clear_only` - uniform (or inside a small noise envelope) and matching the
  expected clear color. Nothing renderer-owned was drawn.
- `uniform_other` - uniform but *not* the expected clear color. This is the
  signature of a full-frame flat-color draw (for example the current
  solid-color world shader), a changed clear color, or uninitialized output.
  It is deliberately never treated as `clear_only`.
- `content` - a meaningful fraction of pixels deviates from the dominant color
  beyond the noise envelope. World geometry, UI rectangles, gradients, or
  texture output is present.

The default expected clear color is the renderer_vrhi clear
`(0.035, 0.055, 0.085)`, normalized to RGB8 `(9, 14, 22)`. Pass
`--no-expected-clear-color` or `expected_clear_color: null` to disable the
expectation.

The controlled manifest `tests/image/clear_content_fixtures.json` pins the
boundary with synthetic frames: the VRHI clear, a full-frame flat world fill,
UI rectangles, thin UI bars, partial world fills, specks below/above the noise
envelope, channel noise, gradients, and checkers, plus threshold-knob cases.
`tests/image/classify_fixtures.py` synthesizes each fixture in memory and
asserts the expected class; its report lands under `temp/image-classify/`.

### Gating renderer slices

The external runner accepts two classification comparison policies per scene:

- `"comparison": "content"` - every capture must classify as `content`. Use it
  to prove a slice actually draws world/UI output instead of leaving the
  backbuffer cleared.
- `"comparison": "clear_only"` - every capture must classify as `clear_only`
  (and match the expected clear color when one is configured). Use it to prove
  a slice has not started drawing yet, or to catch a regression that would
  leave the frame uniform but wrong-colored.

Scenes may also set `"expected_clear_color": "9 14 22"` to override the
renderer_vrhi default. Every successful capture in a report carries
`frame_class` and a `frame_classification` metrics block, so the transition
from clear-only to world/UI output is visible even in report-only runs.

A flat-shaded world fill that covers the whole viewport classifies as
`uniform_other`, which fails both gates by design: it is renderer output but
not yet real world content, and it must never be mistaken for the expected
clear.
