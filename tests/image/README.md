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

Run dependency-free comparator unit tests:

```text
python -m unittest discover tests/image -p "test_*.py"
```

## Current scope

The suite uses the stock `screenshot` command. The demo scene checks pinned
demo playback and requires exact repeatability. Map scenes start a local server,
enable local cheats, move to a pinned spawn/view position, and check
static-world startup. Map captures are currently capture-only because the stock
client path has small timing-dependent scene differences; their raw hashes and
logs are retained for later calibration.
This is a plumbing/repeatability test, not a final-present or
OpenGL-versus-VRHI correctness oracle. The latter requires the opt-in renderer
capture path described in `temp/JOURNAL.md`.

The `.dm_70` OpenArena demos are intentionally not in the manifest because this
ioquake3 build does not support protocol 70. Their maps are covered through
`+map` scenes instead.
