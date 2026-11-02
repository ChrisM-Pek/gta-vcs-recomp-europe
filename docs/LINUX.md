# Linux

The native Linux build: the same source, the same `python3 tools/build.py` pipeline, and the same game as on
Windows. It is built with the distro's own clang and libraries (a plain host build: no container or Flatpak), and it
draws with a Vulkan backend of the RHI (`src/rhi/vulkan`). Decisions: DECISIONS.md, 2026-10-30. The Steam Deck is
postponed; nothing here blocks it.

## Building

```
python3 tools/build.py --check-deps                    # what is missing, and the command that installs it
python3 tools/build.py --recompile "<your disc>.iso"   # disc -> generated C++ -> build/vcs
python3 tools/build.py --quick                         # day-to-day rebuilds (no LTO)
python3 tools/run.py 0 play                            # play (window; build/run/play.log)
```

Needed (the `--check-deps` list, Debian / Ubuntu names):

| What | Packages |
|---|---|
| Compiler: clang 19+, lld, llvm (`llvm-profdata`, `llvm-symbolizer`) | `clang-20 lld-20 llvm-20` (Ubuntu 24.04 / Mint 22; newer from apt.llvm.org) |
| Build tools | `pkg-config git`; `cmake ninja-build` optional: without CMake 3.21+ / Ninja 1.10+ (Debian 11's 3.18, say) the build downloads Kitware's / Ninja's own x86-64 or aarch64 builds into `build/build-tools` (`toolchain.build_tools`) |
| Vulkan shaders (HLSL -> SPIR-V) | `glslang-tools` |
| Movies | `libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev` |
| SDL3, built from source: window (X11, Wayland), audio, gamepads | `libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libpipewire-0.3-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev` |
| Optional: Vulkan validation (`PS2X_RHI_DEBUG=1`) | `vulkan-validationlayers` |
| The launcher (`build.py --launcher`), on top of the above | `libvulkan-dev libfreetype-dev libfontconfig-dev` (X11, Wayland, xkbcommon, D-Bus: already listed) |

Fedora / Arch get the right package names from `--check-deps`. A Vulkan 1.3 driver is needed to play (Mesa's RADV /
ANV, NVIDIA's driver): `vulkaninfo --summary` shows it. The Vulkan headers, volk (the loader) and VMA come with the
build (FetchContent), so no Vulkan SDK is needed.

- **The compiler is clang only** (`tools/toolchain.py` takes the newest LLVM 19+ from `/usr/lib/llvm-<N>/bin` or
  PATH). It is the same compiler as clang-cl on Windows, so the generated code and its numerics are the same; and
  `PS2_GUEST_CC` needs its `preserve_none`. GCC is not supported.
- **The flags match clang-cl's** (`ps2xRuntime/cmake/ReleaseMode.cmake`): `-march=x86-64-v3` (= `/arch:AVX2`; for an
  older CPU `-march=sandybridge` = `/arch:AVX`, or x86-64-v2 - `PS2X_CPU`, picked from `/proc/cpuinfo`),
  `-ffp-contract=off` (the PS2 rounds a multiply-add twice: DECISIONS.md 2026-10-22), `-fno-math-errno`,
  **`-fno-strict-aliasing`** (clang-cl's default; the runtime and generated code read guest memory through casts), no
  stack protector or CET (Ubuntu turns them on).
- **Memory**: a unity batch of generated code takes up to ~2 GB to compile. CMake allows one compile job, and one
  ThinLTO job at the link, per 2 GB of RAM (at most one per core): `VCS_COMPILE_JOBS` and `PS2X_LTO_JOBS` in the CMake
  cache override this.
- The game is `build/vcs` and its settings are `build/vcs.ini`. Saves and caches go to `$XDG_DATA_HOME/VCSRecomp`
  (`~/.local/share/VCSRecomp`); `VCS_DATA_DIR` works as on Windows.

## The launcher

Players get it as an AppImage with every release (`VCS-Recomp-<v>-x86_64.AppImage`, glibc 2.35+: Ubuntu 22.04,
Debian 12 and newer; docs/LAUNCHER.md "The Linux release"); it puts the release's files in
`~/.local/share/VCSRecomp/install` and builds the game there. On Debian / Ubuntu releases whose clang is older than 19
the package step adds LLVM's repository (apt.llvm.org) first (`tools/linux_packages.py`).

`python3 tools/build.py --launcher` builds `VCSLauncher` (repository root): the same launcher as on Windows - setup,
build, play - native on X11 and Wayland, drawing with Vulkan and FreeType (docs/LAUNCHER.md "Linux"). Its build tools
step installs missing packages through `pkexec` (`python3 tools/build.py --install-deps`, the desktop's password
prompt) or opens a terminal with the command. `VCS_LAUNCHER_VIDEO=x11|wayland`, `VCS_LAUNCHER_VK_DEBUG=1` and
`VCS_LAUNCHER_VK_DEVICE=<name>` are its switches.

## Running

- The renderer is Vulkan (`[present] Vulkan on <GPU>` in the log). `VCS_RHI=null` draws nothing: the null backend,
  for checking the CPU side on its own. `VCS_VK_DEVICE=<number | part of the name>` picks the GPU when there are
  several.
- **Hidden test runs** (`PS2X_HIDDEN=1`, everything in `tools/` uses it) make no swapchain at all: the back buffer is
  a texture. Presenting to a hidden window can block forever under Wayland (FIFO waits for a frame callback that never
  comes), and screenshots come from off-screen captures anyway.
- SDL picks Wayland when it runs, X11 otherwise. `SDL_VIDEO_DRIVER=x11` / `wayland` chooses for one run.
- The control channel (`tools/gamectl.py`, the rig) is the same TCP port. The rig copies cards with `shutil` (Linux
  keeps no settable creation time; the card's `.ps2x-order` file holds its directory order) and stops a hung game
  through its process group.

## Debugging

- **Crashes**: SIGSEGV / SIGBUS / SIGILL / SIGFPE print `[crash]` lines with each frame as `module+offset (symbol)`,
  and an `llvm-symbolizer -e <module> <offsets>` line that gives files and lines (`src/debug/crash.cpp`; the exe is
  linked `-rdynamic`, and the runtime and our code carry line tables). The process then dies of the signal itself, so
  the exit status and core dumps stay as they would be. For anything harder: `gdb --args build/vcs <iso>`
  (`tools/minidbg.py` is Windows only).
- **Profiler**: `VCS_PROFILE=5` works as on Windows (`src/debug/profiler.cpp`: one SIGPROF per sample instead of
  SuspendThread), including `VCS_PROFILE_THREAD`, `VCS_PROFILE_SPIKES` and `VCS_PROFILE_GUEST_FILE`.
  `VCS_PROFILE_CALLERS` and `VCS_PROFILE_LINES` are Windows only: `perf record -g build/vcs <iso>` covers them.
- **Vulkan**: `PS2X_RHI_DEBUG=1` turns on the Khronos validation layer and prints its messages as `[rhi] vulkan`.
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` adds the synchronization checks. `PS2X_RHI_STATS=1` prints draws,
  descriptor writes, passes, suspended passes, barriers, upload volume and the CPU's waits (fence, acquire, submit,
  present) per frame every 5 s. `PS2X_GPU_TIMING=1` gives GPU time per marker as on D3D11 (gs_hw breaks the pass at
  every draw for it); `PS2X_RHI_GPU_TIME=1` times the passes as they are and the whole frame.
- **Benchmarks**: run without the control channel (`PS2X_PERF_LOG` gives the [perf] lines): the control channel turns
  on the brightness probe, a full GPU wait every frame (PERFORMANCE.md 16).

## Differences from Windows, and why

| Area | Windows | Linux | Why |
|---|---|---|---|
| Renderer | D3D11 (reference) | Vulkan | D3D11 is Windows only. The Vulkan backend builds on Windows too (`-DVCS_RHI_VULKAN=ON`, then Graphics > Graphics API), to compare it with D3D11 on the same PC |
| Shaders | fxc -> DXBC | glslang -> SPIR-V (`tools/hlsl_spirv.py`) | One HLSL source. glslang ignores `[[vk::index(1)]]`, so the script moves the second output to Location 0 Index 1 itself. `VCS_SPIRV` adds a point size, which Vulkan needs for points |
| Dear ImGui | `imgui_impl_dx11` | `render/ui_renderer.cpp` (through the RHI) | One pipeline for every target format (the swapchain's BGRA, the RGBA captures) |
| Write watch (`ps2_write_watch.cpp`) | `GetWriteWatch` (dirty bits: free writes) | userfaultfd async write-protect + `PAGEMAP_SCAN`, **opt-in** (`PS2X_WRITE_WATCH=1`) | Correct (0 wrong skips), but each first write to a page after a fold is a page fault on the game thread: no net gain measured (PERFORMANCE.md 20) |
| Card creation time | `GetFileAttributesExW` | `statx` birth time (mtime without one) | Only the order of new entries; `.ps2x-order` is the source of truth |

## Work list

| Step | Status |
|---|---|
| Toolchain: clang 19+ found, flags as clang-cl, RAM-capped jobs, `--check-deps` | done (2026-10-30): builds with clang 20 on Mint 22.3. Ubuntu's clang-20 picks GCC 14's folder, which has no libstdc++: `toolchain.py` passes `--gcc-install-dir` of the newest GCC that has one. SDL's XTest is off (one package less). libstdc++ needed a few includes MSVC's STL brought in |
| Recompile on Linux; `generated/` the same as on Windows | recompiles (10,387 files); tree hash `0f27b7e8...` (CRLF -> LF, names + contents of generated/ee and vu1) - to compare with a Windows recompile |
| Host / runtime fixes: exe folder, XDG data folder, `_BitScanForward`, `fseek` sign on LP64, IOP `strtol` 32-bit, control socket SIGPIPE / reuse, debug trap, card birth time, rig kill / copy | done (2026-10-30), syntax-checked with GCC 13 |
| Null RHI backend (`VCS_RHI=null`) | done, not run in the game yet |
| Vulkan RHI backend + SPIR-V shaders + RHI ImGui renderer | runs (2026-10-30, HD 520 / Mesa ANV, X11): title flyover and intro cutscene look right; 27-29 fps at the 30 fps cap in a quick build. Fixed on the way: bindings kept pointers to destroyed buffers (D3D11 keeps a reference) - destroyed objects now unbind themselves |
| Bit-exact checks on Linux: `PS2X_HLE_VERIFY=1`, `PS2X_VU0_VERIFY=1`, `--iop-tests` | VU0: 0 differences in millions of runs of every program; SPU2 tests pass; HLE (150 s from an empty card: flyover, intro): setup programs, re-draws, C++ clipper, env pass exact; world fast path 8 packets within 1 and 11 off of 2.46 M (103 vertices, ST / XYZF) - the same run on Windows is needed to tell whether that is Linux's |
| Vulkan validation (`PS2X_RHI_DEBUG=1`, 70 s flyover + intro) | clean: 0 errors, 0 warnings; 1-2 suspended passes a frame (in-pass copies), 0 waits |
| Picture: Windows D3D11 vs Windows Vulkan, Windows reference vs Linux Vulkan (`scenes.py compare`) | to do |
| Crash handler (signals, module+offset, llvm-symbolizer line) | done, tested in a stand-alone program |
| Sampling profiler (SIGPROF) | works in the game (`VCS_PROFILE_THREAD=MainThread`, PERFORMANCE.md 16) |
| Write watch: userfaultfd `WP_ASYNC` + `PAGEMAP_SCAN` (Linux 6.7+) | done, opt-in: correct, no net gain on the laptop (PERFORMANCE.md 20) |
| PGO on Linux (training from a new game when the rig's saves are missing) | done: no measurable gain over ThinLTO on the laptop (PERFORMANCE.md 20) |
| Input delay measured and cut (host-paced frames, frame lead) | done: 60 cap 50 -> 34 ms (PERFORMANCE.md 19) |
| The scheduler's guest-thread switches (C++ throws) on Linux vs Windows: `bench.py` | to do |
| GPU timing (`PS2X_GPU_TIMING`) for Vulkan: timestamp queries | done (and `PS2X_RHI_GPU_TIME`) |
| Vulkan optimisation pass: layout-based barriers, incremental push descriptors; spirv-opt measured (no gain) | done: 60 fps locked at a 60 cap on the HD 520 (PERFORMANCE.md 16) |
| The rig on Linux: regress, tour, saves, trainer, controls, shakedown, finale, soundcheck, soak, framerate audit | to do |
| The launcher: X11 + Wayland, Vulkan, FreeType, portal dialogs, `--install-deps`; CI job on Ubuntu 24.04 (lavapipe) | done (2026-11-01): checked in nested Xephyr / weston (docs/LAUNCHER.md "Checked (Linux)"); GNOME / KDE by hand to do; a Linux release to do |
| The Windows stutter work (PERFORMANCE.md 21-24) on Linux | done (2026-11-02, PERFORMANCE.md 25): portable, in; Vulkan's free-threaded texture creation added, off by default (no gain on the laptop; `PS2X_HW_EARLY_TEXTURES=1`); `cruise.py` from a copy of the player's card |
| Hitches driving in the city on the laptop (PERFORMANCE.md 26) | measured: the main thread's driver CPU per draw and the GPU, both at the 60 Hz budget in dense views. Done: no shader depth on Vulkan (early Z), constants written only when they change, redundant Vulkan state not recorded (GPU -10%, main thread -10-15%); then (PERFORMANCE.md 27) depth as dynamic state (pipeline binds -60 to -70%) and vertex uploads of the written bytes only: main thread -12 to -16%, its frames over 20 ms 61 -> 24 |
| Performance on this laptop (i5 Skylake, HD 520: the "modest hardware" target) in PERFORMANCE.md | 30 locked (original timing), 60 locked at a 60 cap, ~83 shown fps uncapped (frame-rate independent; PERFORMANCE.md 16-18) |
