# macOS

The native macOS build: the same source, the same `python3 tools/build.py` pipeline and the same game as on Windows
and Linux, for **Apple Silicon** (M1 and newer). The game and the launcher draw with **Metal** (`src/rhi/metal`,
`launcher/render/metal`). Decisions: DECISIONS.md, 2026-11-12. Intel Macs are not supported.

## Building

```
python3 tools/build.py --check-deps                    # the Command Line Tools there?
python3 tools/build.py --recompile "<your disc>.iso"   # disc -> generated C++ -> build/vcs
python3 tools/build.py --quick                         # day-to-day rebuilds (no LTO)
python3 tools/build.py --launcher                      # VCSLauncher.app in the repository root
python3 tools/run.py 0 play                            # play (window; build/run/play.log)
```

The only thing to install is **Apple's Command Line Tools** (`xcode-select --install`, or Xcode): Apple clang 17+
(LLVM 19: `preserve_none`, checked by CMake), Apple's linker (ld64), `make`, `git` and `python3`. Everything else is
fetched or built by the build itself, pinned, and bundled by `tools/deps.py --platform macos` for offline builds:

| What | How |
|---|---|
| CMake, Ninja | any on PATH (Homebrew's, CMake.app), else Kitware's universal tarball / Ninja's mac zip into `build/build-tools` |
| glslang, SPIRV-Cross (the Metal shaders) | built once from `tools/shader_tools/CMakeLists.txt` into `build/build-tools/shader-tools` (`tools/shader_tools.py`, ~1 min). `VCS_GLSLANG` / `VCS_SPIRV_CROSS` take other copies |
| FFmpeg (movies) | FFmpeg 7.1.2's source release with only the MPEG-2 decoder, its parser and swscale, static and LGPL (`ps2xRuntime/CMakeLists.txt`, ~1 min once per build tree). `-DVCS_FFMPEG=system` takes pkg-config's (Homebrew's) |
| SDL3, Dear ImGui, zlib, sse2neon, ... | FetchContent, as on the other platforms |

- **The compiler is Apple's clang only** (`toolchain.macos_compiler_defines`: `xcrun -f clang`). Homebrew's LLVM is
  never taken: its bitcode can't go through Apple's linker for ThinLTO.
- **Flags** (`ps2xRuntime/cmake/ReleaseMode.cmake`): `PS2X_CPU=ARM64` = `-mcpu=apple-m1`; `-ffp-contract=off` and
  `-fno-strict-aliasing` in **every** configuration (arm64 always has FMA; the PS2 rounds a multiply-add twice);
  ThinLTO through ld64 (`-cache_path_lto`, `-object_path_lto`). The deployment target is macOS 13 (Metal 3).
- **The SSE code**: the generated C++ is the same as on x86. Its SSE intrinsics run through sse2neon
  (`USE_SSE2NEON`, `third_party/PS2Recomp/CMakeLists.txt`) with `SSE2NEON_PRECISE_MINMAX / DIV / SQRT / DP`: PS2
  floats with an all-ones exponent are NaN on the host and go through `minps` / `maxps`, whose NaN rule the precise
  path keeps. The rounding mode is FPCR instead of MXCSR (`ps2x::fpControl`, `runtime/ps2_host_std.h`).
- The game is `build/vcs`, its settings `build/vcs.ini`. Saves and caches go to
  `~/Library/Application Support/VCSRecomp`; `VCS_DATA_DIR` works as everywhere.

## Running

- The renderer is Metal (`[present] Metal on Apple M1` in the log; `[rhi] graphics cards, fastest first:` lists the
  GPU). `VCS_RHI=null` draws nothing (the CPU side alone). The window is a Retina window: it draws at the screen's own
  pixels.
- **Shaders** are the one HLSL source as MSL text (`tools/hlsl_msl.py`: glslang -> SPIR-V, with the dual-source patch
  of `hlsl_spirv.py` -> SPIRV-Cross), compiled by the OS at run time with fast math off (`MTLMathModeSafe`) - the
  Command Line Tools have no Metal compiler. All of the game's 39 take ~50 ms on an M1.
- **Hidden test runs** (`PS2X_HIDDEN=1`) draw into an offscreen back buffer, as with Vulkan.
- **Threads**: macOS gives a `std::thread` 512 KB of stack. The game thread, the render worker and the GS stage are
  `ps2x::Thread`s with 16 MB, and ask for QoS user-interactive (performance cores).

## Debugging

- **Metal validation**: `MTL_DEBUG_LAYER=1` (add `MTL_DEBUG_LAYER_ERROR_MODE=nslog` to log instead of stopping).
  `PS2X_RHI_STATS=1` prints draws, passes, suspended passes and upload volume per frame every 5 s
  (`[rhi] metal per frame: ...`).
- **Crashes**: SIGSEGV / SIGBUS / SIGILL / SIGFPE print `[crash]` lines with each frame as `module+offset (symbol)` and
  an `atos -o <module> -l <load address> <addresses>` line that gives files and lines (`src/debug/crash.cpp`). Or
  `lldb -- build/vcs <iso>`.
- **Profiler**: `VCS_PROFILE` is not on macOS yet (a stub; Instruments' Time Profiler covers it).

## The launcher

`VCSLauncher.app` (`python3 tools/build.py --launcher`, ~20 s plus the shader tools the first time): Cocoa, Metal and
Core Text, no third-party code (docs/LAUNCHER.md "macOS"). The window keeps macOS' traffic lights over the UI's own
title bar. Its Build tools step installs the Command Line Tools (`build.py --install-deps` runs `xcode-select
--install`). `--self-test` and `--screenshot` work as on the other platforms.

## Differences from Windows / Linux, and why

| Area | Windows | Linux | macOS | Why |
|---|---|---|---|---|
| Renderer | D3D11 | Vulkan | Metal | Each platform's own API; Vulkan on macOS would need MoltenVK |
| Shaders | DXBC (d3dcompiler_47) | SPIR-V (glslang) | MSL text (glslang + SPIRV-Cross), compiled at run time | One HLSL source |
| SSE | native | native | sse2neon (precise) | arm64 |
| Rounding mode | MXCSR | MXCSR | FPCR | arm64 |
| Write watch | `GetWriteWatch` | userfaultfd, opt-in | off | No gain measured on Linux; 16 KB pages here |
| Card creation time | `GetFileAttributesExW` | `statx` | `st_birthtimespec` | The card's directory order |
| FFmpeg | prebuilt (LGPL) | the distribution's | built from source (movie decoder only) | Nothing to install beyond Apple's tools |

## Work list

| Step | Status |
|---|---|
| Toolchain: Apple clang + ld64, ARM64 CPU level, CMake / Ninja downloads, `--check-deps` / `--install-deps` | done (2026-11-12) |
| FFmpeg and the shader tools from pinned sources | done |
| Recompile on macOS; the game builds (`--quick`) | done: 10,386 generated files, unchanged; only our own code needed the SSE include |
| Runtime: FPCR, thread stacks, exe / data folders, card times, crash handler | done |
| Bit-exact checks: `PS2X_VU0_VERIFY=1`, `PS2X_HLE_VERIFY=1` (150 s, flyover + intro) | done: VU0 0 differences in millions of runs; HLE setup programs, re-draws, C++ clipper, env pass exact; world fast path 11 within 1 / 14 off of 2.56 M - as Windows' llvm-mingw reference |
| Metal RHI backend | runs: title, flyover and intro cutscene draw right, 60 fps at a 60 cap on an M1 (~1,500 draws, 28 passes, 1 suspended pass a frame); validation shows no errors |
| The launcher: Cocoa, Metal, Core Text | done: self-test, screenshots, the window |
| ThinLTO build (`--opt lto`) with ld64 | done: 3.5 min for the whole game on an M1 (8 cores, 16 GB); plays (intro cutscene: game 4.1 ms a frame) |
| Picture: Windows D3D11 reference vs macOS Metal (`scenes.py compare`) | to do (needs a reference capture from Windows) |
| `--iop-tests` on macOS | done: 0 failures |
| The rig on macOS: regress, tour, saves, trainer, controls, hud, menu, shakedown, haze, soundcheck, soak | to do |
| ARM memory ordering: a ThreadSanitizer run and a review of the relaxed atomics on cross-thread paths | to do |
| Performance: `bench.py`, `cruise.py`; NEON path for `vu1v`; `preserve_none` on arm64 (measure); fewer suspends | to do |
| Profiler (Mach thread sampling) | to do |
| Release: `release.py --platform macos` (an ad-hoc signed `VCSLauncher.app` in a zip), `update.py`, a CI job | to do |
