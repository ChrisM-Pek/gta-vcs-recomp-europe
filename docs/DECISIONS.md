# Decisions log

Newest at the bottom. Each entry: what, why, and what would change it.

## 2026-10-21 - Base toolchain: vendored PS2Recomp
- PS2Recomp (ran-j, GPL-3.0) is vendored into `third_party/PS2Recomp` at upstream
  commit in `third_party/PS2Recomp/UPSTREAM_COMMIT`, without its `.git`,
  `android/` and `vita/` folders. Vendored (not a submodule) because we will patch
  the runtime heavily and can't publish a fork from here. Keep local changes
  small and listed in `docs/PS2RECOMP_PATCHES.md` so they can be upstreamed.
- The project is GPL-3.0 as a result.

## 2026-10-21 - Distribution: source only, bring your own disc
- The repo never contains the ISO, the ELF, or C++ generated from the ELF
  (`.gitignore` blocks them). Users run the pipeline on their own disc.
- The built exe reads game files straight from the user's ISO at runtime.
- Only one executable build is supported (SHA-1 in `tools/vcs_disc.py`).

## 2026-10-21 - Performance is a hard requirement
- Upstream runtime uses raylib + a CPU software GS renderer, which the upstream
  README calls "very bad" for speed. That's fine for bring-up only.
- Target: runs full speed on old/modest hardware (dev box: i7-7700K + GTX 1080 Ti,
  treat it as the *high* end). So:
  - GS: hardware-accelerated renderer (Vulkan/D3D), not a CPU rasterizer and
    preferably not a heavy compute rasterizer either. paraLLEl-GS is an option
    for reference accuracy, but the end state is a native renderer fed directly
    from the game's draw calls (HLE of VU1 programs).
  - VU1: translate the game's microprograms to C++ ahead of time, never interpret.
  - Disc I/O: memory-map the ISO, no per-read file opens.
  - Avoid per-instruction runtime dispatch in hot paths (e.g. indirect jumps
    through lookup tables) where we can resolve statically.

## 2026-10-21 - Function map: Ghidra functions only, no label records
- Ghidra's export script also emits every branch-target label (57,897 of them)
  as a separate "function". With those, ps2_recomp produced 67k files / 572 MB
  with each code chunk duplicated. Without them: 9,584 files / 243 MB, 0 errors.
- ps2_recomp still discovers internal entry points itself and maps each to its
  owner function (the function starts with a `switch (ctx->pc)` that jumps to the
  resume label), so jump tables and thread resumes stay inside one C++ function.
- BlackLine's archived VCS map (`gta.csv`) is for the PAL build SLES-54622, so its
  addresses don't apply to SLUS-21590. Its function count (9,549) matches ours,
  which is a good sanity check that Ghidra's boundaries are complete.

## 2026-10-21 - Compiler: clang-cl + MSVC 2019 STL
- The machine's VS 2022/2026 installs have no C++ toolset; VS 2019 Build Tools
  (MSVC 14.29) do. We build with LLVM clang-cl 23 against that STL and the
  10.0.19041 Windows SDK. clang-cl also handles the huge generated files better
  than cl.exe. `tools/toolchain.py` sets this up (vswhere + vcvars64).
- Baseline CPU: AVX2 (upstream's `/arch:AVX2`). Revisit if we want pre-Haswell
  CPUs; the generated code mostly needs SSE4.1. (Revisited 2026-10-31: the build follows
  the PC's CPU - AVX2, AVX or SSE4; see the entry of that date.)

## 2026-10-21 - Graphics plan
1. Bring-up: upstream CPU GS backend (slow, but no extra work).
2. Accurate + fast enough: paraLLEl-GS (Vulkan compute, LGPLv3+) for exact
   output with supersampling, as hctp-recomp does.
3. End state for performance and real hi-res/widescreen: native renderer fed by
   high-level hooks on the game's VU1 render paths (VCS has only ~4.8 KB of VU
   microcode + 4 small DVP overlays, so the set of programs is small).
- 2026-10-22: step 3 is now planned in detail in NATIVE_RENDERER.md (SDL3 platform
  layer, own RHI with a D3D11 backend, GS HW backend port, then packet-level HLE).
  Its six decisions were approved by the user on 2026-10-22.

## 2026-10-22 - Movies skipped during bring-up
- `FUN_0021CA70` (from `main`) plays `STINGS.PSS` then `INTRO.PSS` via the movie
  player at `0x40D8B8`. Under the runtime's sceMpeg stubs it stalls after ~8
  stream reads. Skipped (returns 0) in `src/game/vcs_overrides.cpp` for now.
- Plan (Phase 4): native playback - demux the PSS from the disc with FFmpeg and
  draw it ourselves (cheaper than emulating the IPU, and allows clean scaling).

## 2026-10-22 - Iteration speed
- New bindings go in `src/game/vcs_overrides.cpp` (relink only) rather than the
  recompiler config (rebuilds all generated code).
- LTO off for development builds; ThinLTO for release builds.

## 2026-10-22 - Rule for hooks on recompiled functions
- A generated function can return to the EE scheduler in the middle of its body
  (thread switch at a checkpoint, e.g. inside a `DelayThread` poll loop). It is
  later resumed through the function table at the *resume address*, which maps
  to the original generated function, not to a wrapper registered on its entry.
- So a hook may do work **before** calling the original, or replace the
  function entirely, but must not rely on code running after the original
  returns. For "after" logic, hook the next function the caller invokes.

## 2026-10-22 - paraLLEl-GS: integrated, but off by default
- Ported hctp-recomp's paraLLEl-GS backend (runtime/gs/gs_gpu*.{h,cpp}) and hooked
  it into upstream's GS frontend (raw GIF per path, register writes, IMAGE uploads,
  readbacks, present at VBlankStart, frames read back into the raylib window).
- On the dev GPU (GTX 1080 Ti, Pascal) every run hung the GPU > 2 s and Windows
  reset the display driver (TDR, event 4101); NVIDIA's OpenGL driver then
  aborts the process (fail-fast code 7 in nvoglv64.dll). Independent of the
  supersampling rate. Likely the compute rasterizer relies on behaviour Pascal
  lacks (no independent thread scheduling) or on a content-specific hang.
- Because the target is *modest* hardware, a compute-heavy rasterizer that
  doesn't run on a 2017 high-end card is the wrong default. The CPU renderer
  stays default; `PS2X_GS=gpu` opts in to paraLLEl-GS for newer GPUs.
- Next: a conventional hardware-rasterized GS backend (GSdx-HW style) behind the
  existing `GSRasterBackend` interface - broad GPU support, upscaling-friendly.

## 2026-10-23 - Post effects: exact GS emulation at 640x448 first, resolution-aware reads for higher resolutions

The user asked for the post effects to stay as authentic as possible at the original resolution first, then at
higher resolutions and other aspect ratios. At 640x448 the emulated GS draws are the reference, so the first step
made them match the GS (pixel centres) instead of replacing them with approximations, and kept them correct at
higher internal resolutions by reading 1:1 copies at full resolution. Effect-level shaders (one pass per effect,
independent of the PS2 frame-buffer layout) come with the native frame (milestone 3 step 4), checked against this
emulation at 640x448.

## 2026-10-23 - Native HUD: the game names its primitives, the GS packets still carry them

Milestone 3 step 2 draws the HUD natively (docs/NATIVE_RENDERER.md "HUD layer"). Choices:
- **The game's own Im2D calls still run** and build their GS packets; the renderer takes the HUD's primitives out of
  the frame by matching them against the Im2D calls of the frame (vertex positions), then draws them at the window's
  size. Replacing Im2D outright would also need RenderWare's texture cache (VRAM placement, CLUT uploads) and its
  render-state translation reproduced; going through the packets keeps those exact and leaves a switch back
  (`VCS_HUD_NATIVE=0`) at no cost.
- **Textures from the GS backend's decoded cache**, not a decoder for RenderWare rasters: the HUD textures reach VRAM
  through the game's own uploads, and the cache keys them by content. A raster decoder becomes worth having when 2D is
  drawn without GS packets (milestone 3 step 4, a native frame).
- **Anchors from where an element sits** (grouped boxes, the 4:3 screen in thirds with a dead zone around the middle),
  not a table of HUD call sites: it covers text boxes, timers and counters the game positions itself, with nothing to
  keep in step with the code. Elements may change side as they grow (a help box past the middle stays left: the
  rule uses its middle and its left edge).
- The HUD placement **default stays "Keep 4:3"**; "Screen edges" is a choice in the menu (authentic by default).

## 2026-10-23 - Clipped world packets: correct picture first, then the C++ clipper

GPU clipping of the packets the VU sends through its clipper drew GON_C3's poolside ground black. The user chose the
correct picture as the default (clipped packets on the VU1, flyover 30 -> 25.5 fps) and milestone 3 to start with the
VU clipper. It is reproduced bit-exactly in C++ (game-internals.md 6h); its GIF packets go to the GS as the VU's
kicks would, so the picture is the VU's and the cost is close to GPU clipping. GPU clipping stays behind
`PS2X_HLE_CLIPPED=1`.

## 2026-10-22 - Performance status (i7-7700K)
- With GS work discarded (`PS2X_GS=null`) the game now runs at full speed
  (60 VBlanks/s, 15% idle) after two changes: hctp's VU1 throughput mode
  (13 -> 23 VBlanks/s) and batching IOP time (23 -> 60).
- With the CPU GS renderer it is 2.9 VBlanks/s: the rasterizer is ~95% of the
  frame. A GPU GS backend is the critical path.

## 2026-10-22 - VU1 microcode compiled ahead of time
- VCS uploads 22 distinct VU1 microcode blocks at run time (REF payloads from
  `.text`, the MPG VIFcode in the DMAtag). `game/vcs/vu1_uploads.txt` lists their
  ELF addresses; `tools/vu1_recomp.py` (vendored from BigDaddyJRich/hctp-recomp,
  GPL-3.0, plus an uploads-list input) reads the bytes from the user's ELF at
  build time and writes `generated/vu1/vu1_compiled.cpp` (never committed).
- Pieces are split at every upload boundary because VCS overlays parts of its
  big block with per-pass variants and a piece runs only while VU memory holds
  all of its bytes. Result: 99.97% of VU1 pairs run compiled.
- Verified with `PS2X_VU1_VERIFY=1`: registers, VU memory and XGKICK packets match
  the accurate interpreter; only cycle counts differ (throughput variant).
- Frame rate in game (i7-7700K): 11 -> ~35-50 VBlanks/s over this pass
  (compiled VU1, CLUT hash cache, content-keyed textures, upload fast path,
  batched timers). The EE thread is the single saturated thread. Next: VU0
  microprograms 0x0CF8 (sector box clip) and 0x0EB8 (~25% of the EE thread;
  they read MAC flags with pipeline timing, so they run on the exact path).

## 2026-10-22 - Native VU0 programs; no FP contraction
- VU0 programs 0x0CF8 (sector box clip) and 0x0EB8 (3x matrix product) ran
  ~160k times / 5 s on the interpreter's exact path (they read MAC flags):
  ~25% of the EE thread. They are now C++ (`src/game/vcs_vu0_native.cpp`),
  transcribed pair by pair with the flag latency spelled out, using the same
  per-lane maths as the compiled VU1 code. Registered by code hash.
- Verification (`PS2X_VU0_VERIFY=1`) first showed 1-ulp differences: the
  *interpreter* was wrong - /arch:AVX2 let clang fuse `acc + a * b` into FMA
  (one rounding). The PS2 rounds product and sum separately. The runtime and
  runner now build with -ffp-contract=off; after that, 4.2 million live calls
  matched exactly (VF, VI, ACC, VU0 memory), and compiled VU1 still matches.
- In game (i7-7700K): ~46-67 VBlanks/s (was 35-50 before, 11 at the start of
  the performance work). Remaining top costs: generated game code ~31%, T4
  texture swizzle (WriteP4) ~9%, DMA chain copy (memmove) ~7.5%, HW backend
  recording ~7%, scheduler checkpoints ~4%.

## 2026-10-22 - SPU2 emulated in the IOP; IOP time follows the wall clock
- The SPU2 lives in ps2xIOP (`core/iop_spu2.cpp`), behind the IOP's memory map
  and DMA channels 4/7, rather than as a high-level libsd replacement: VCS's
  CDSTREAM.IRX drives libsd directly, and its streaming depends on SPU2 IRQ
  addresses and DMA-complete callbacks, which an HLE layer would have to fake.
- The mix (48 kHz stereo, one frame per 768 IOP cycles) goes to a raylib audio
  stream through a ring buffer with an adaptive target fill (60 ms, +30 ms per
  underrun, max 250 ms). Drift is absorbed by resampling within +-0.5%. A first
  version resampled by up to +-8%: when the buffer ran low the music audibly
  slowed down, which the user heard as "audio slows down with the frame rate".
- IOP time was derived only from accounted EE cycles, so when the EE ran below
  full speed the SPU2 produced 70-90% of real-time audio and playback stuttered
  in step with the frame rate. On hardware the IOP/SPU2 keep real time whatever
  the EE does, so each IOP batch now brings IOP time up to the host clock (at
  most 50 ms at once; lag > 1 s is forgiven). Measured: 100% production and 0
  underruns at 20-60 VBlanks/s (slow host simulated with PS2X_SLOW_VBLANK_MS).
  Stream refills (DMA bytes, IRQs per second) are the same at 20 and 60
  VBlanks/s: CDSTREAM feeds music from the IOP side, not per game frame.
  `PS2X_IOP_REALTIME=0` turns it off.

## 2026-10-22 - Native port roadmap decisions (docs/NATIVE_PORT_ROADMAP.md)
The user's choices on the roadmap's open questions:
- Start with milestone 1 (automation / coverage rig), then milestone 2 (correctness sweep), and
  carry the roadmap out autonomously, committing at milestones and major changes.
- Movies: FFmpeg as the decoder.
- 60 fps: a settings-menu option, off by default (30 fps stays the authentic default). (Superseded 2026-10-30: 60 fps
  with frame-rate independent timing is the default.)
- IOP retirement (milestone 6): do it.
- Windows only for now; milestone 9 (portability / distribution) is out of scope until further notice. (Superseded
  2026-10-30: native Linux, see that entry.)
- Game knowledge goes into docs/game-internals.md (successor of ENGINE.md, each fact with how it was
  found); AGENTS.md is the working contract for agents.

## 2026-10-23 - Render submission on its own thread; frame-rate independence next
- The user's order: the render submission worker first, then frame-rate independence (a variable time step:
  "remove dependence on the framerate for gameplay"), not fixed-step interpolation. The worker comes first because
  with game code and render submission in series on one thread (~18 + ~17 ms driving) a variable step would barely
  raise the frame rate, and one large risky change at a time keeps bugs attributable.
- Design (runtime/ps2_render_worker.h): the game keeps its synchronous view - chains are copied when kicked and the
  DMA completes at once, as before - and the work runs behind it in kick order. Everything the game can read back
  fences (waits for the worker) instead of being made asynchronous; measured, the game only reads the GS CSR (~5 a
  frame), so fences cost little. Rare game-thread entries into the GS (library stubs, native GIF fast paths) fence
  or step aside rather than being ported to the queue. At most one frame of lead (latency stays one frame).
- Kept outside ps2_memory.h (a free-function module with its own buffer pool) so the change does not rebuild the
  generated code. `PS2X_RENDER_THREAD=0` is the switch back.

## 2026-10-23 - Profile-guided, ThinLTO build for play; plain build for development
- `tools/build.py --opt pgo` (ThinLTO + PGO) is what to play and measure with: game code ~20-30% faster
  (PROGRESS.md milestone 4). The profile comes from an instrumented build (build/pgo-gen) playing tools/bench.py and
  the reference scenes; it lives in build/pgo (derived from the user's own game, never committed) and is retrained
  with `--opt pgo --train` after large changes - a stale one still builds, changed functions just lose its benefit.
- `--opt none` stays the development setting (no ThinLTO relink); the choice is kept in the CMake cache until changed.
  (2026-10-27: a fresh build directory is now `lto`, with `--quick` for `none` - see that entry.)
- Calls between recompiled functions got an inline fast path in the runtime header rather than a recompiler change:
  same effect on every call site, no regenerated code, and `PS2X_FAST_DISPATCH=0` switches it off.

## 2026-10-25 - Independent timing up to 240 fps; the ped-movement branch below step 0.5 patched
- Found what broke below the 0.5 step (docs/PERFORMANCE.md 5, game-internals.md 4): FUN_001f1d38 turns a ped's
  animation movement into a velocity with `* 0.5` instead of `/ step` when the step is under 0.5 - a branch the PS2
  never takes (CTimer floors the step). One instruction patch (game/vcs/overrides.toml: the constant it compares with
  becomes 0.001) sends every real step down the `/ step` path; steps of 0.5 or more run exactly as before.
- The audit then matches 1/30 - 1/100 at 1/144, 1/200 and 1/240 (running, driving, falling; sprint -5% / -11% at
  1/200 / 1/240), so independent timing's ceiling goes from 100 to 240 fps (step floor 50/240). Caps offered: 30 / 60
  / 100 / 120 / 144 / 165 / 240 / display refresh / unlimited (both up to 240). Missions above 100 fps are not
  playtested yet; the four frame-counter blinkers outside the 2D pass blink faster as before.

## 2026-10-24 - Independent timing capped at 100 fps for now
- VCS clamps its time step to at least 0.5 (1/100 s; Vice City used 0.01). With the clamp lifted, the frame-rate
  audit shows the player running at a fifth of the speed at 1/120 s - code somewhere assumes steps of 0.5 or more.
  Rather than run the game fast (keeping the clamp above 100 fps) or let it break, independent timing keeps the
  game's 0.5 floor and caps the frame rate at 100. The cap setting offers 30 / 60 / 100 / display refresh (up to
  100) / unlimited (up to 100). Finding and fixing what breaks below 0.5 (then 120 / 144) is phase 3 work;
  `VCS_MIN_STEP` lowers the floor to test it.

## 2026-10-24 - Movies played natively; the intro skippable from the settings
- CMoviePlayer::Play is replaced at its entry rather than feeding the game's own libmpeg / IPU player: no IPU, no
  movie sound through the IOP, the picture drawn by our presenter at 4:3 like the PS2's TV. The game thread waits in
  Play as it did in the game's player.
- Skipping as on the PS2: the logos can't be skipped (the game passes a threshold of 1,000,000 frames), the intro can
  with Start / Cross. The user asked to keep skipping the intro possible: Display > Skip intro movies skips both
  movies; off by default (authentic). Test tools set VCS_SKIP_MOVIES=1 so their boot timelines stay as they were.

## 2026-10-24 - Sound without the IOP: Rockstar's sound code ported onto a native libsd and the existing SPU2 core
- The roadmap's "native mixer" is the SPU2 core we already had (ADPCM, ADSR, interpolation, reverb - in C++, checked
  against hardware behaviour); what was emulated around it was MIPS code: Sony's libsd and Rockstar's CDSTREAM sound
  engine. Those are ported function by function (src/sound/libsd.cpp, src/game/vcs_sound.cpp), so the sound is the
  same by construction and checkable write for write; a different mixer would only have added a second way to sound
  different. A higher-quality mixer can later sit behind the same libsd.
- The IOP's threads became steps on the EE thread (RPC work in the call, the music thread one step per semaphore
  signal, max count 1 as the IRX creates it). Disc reads complete at once; DMA transfers keep their duration (the
  game opens a radio station and sends its volume 3 samples later - it relies on the preload taking that long).
- Tables the module carries (112 stream names, SPU2 addresses) are read from the disc's own MODULES\CDSTREAM.IRX at
  start (checked to be SLUS-21590's), so nothing from the game sits in the source; only code constants (bank first
  sound numbers, slot addresses) are written out.
- Checked by record / replay (tools/soundcheck.py) rather than by ear alone: the emulated IOP's sound RPCs are
  recorded with their IOP times and replayed into the native driver; SPU2 writes and audio are compared.
- With streaming and sound native the emulated IOP is off by default (no modules, no IOP time); `PS2X_IOP=1`,
  `VCS_NATIVE_SOUND=0` or `VCS_NATIVE_STREAM=0` bring it back for checking. The emulated IOP's code stays.

## 2026-10-24 - RSL world draws: record and replay at the VIF level, on by default

Phase 2 of RSL_SUBMISSION.md for world sectors. The world draw's geometry is drawn at a MARK put in place of its REF,
from a recording of what the VIF did with the block the first time (UNPACK results, MSCALs, meshes) rather than from a
decoder of our own:
- **Exact by construction**: the recorded VU qwords are the interpreter's own UNPACK results, the MSCALs run through
  the interpreter's own MSCAL routine (`PS2Memory::vu1Mscal`) and the native world handler; a recording is refused when
  its result could depend on state from before the block. A decoder of the UNPACK formats, masks and modes would have
  been a second implementation to keep in step.
- **The MARK keeps the order**: the draw happens where its REF was, after its state flush and GS register writes, on
  whichever thread processes the chain (render worker or not).
- **Invalidation by content**: a block is hashed each time it is drawn (~5% of the chain's bytes; cheaper than copying
  it) - streaming replaces cells without telling us, and the hash notices.
- **On by default** after the verify run (0 of 174,301 replays differ) and the picture / tour checks; `VCS_RSL_WORLD=0`
  keeps the packet path. The timing gain is within noise (world geometry is small); the path is the point - model
  instances come next on the same mechanism.

## 2026-10-24 - RSL model draws: the same record / replay; marked bytes stay in the chain's copy

Model instances (`FUN_002F7D58` / `FUN_002F80A0`) use the world's mechanism, with two changes that apply to both:
- **The marked bytes are copied and skipped, not replaced** (revises "cheaper than copying it" above): the render
  worker processes a chain while the game builds the next frame, so reading a block from guest memory at the MARK
  could see a cell or model streamed out after the kick. The copy is what the guest held at the kick; copying costs
  little next to processing. It also gives a CALLed packet - a sub-chain of tags - as one flat stream without a second
  chain walker of our own.
- **Every CALL in the packet buffer is marked**, not only the ones the hooked draw calls wrote: a recording is the
  interpreter's own result for those bytes, so it is exact whoever built the packet; the hooks only label the
  counts. Packets rebuilt every frame drop to the packet path by themselves (bytes changed three hits running).
- On by default after the verify run (1,217,887 blocks, 0 differ), scenes and tour; worker -1.3 to -2.4 ms in the
  bench. `VCS_RSL_MODELS=0` keeps the packet path for models.

## 2026-10-24 - Game thread: the profile's targets before draw-call replay

The plan after phase 2 was to replay draws at the RSL draw call instead of building their packets. A drive profile
with call chains showed the draw calls' own writes are a small part of the game thread; a reallocating chain copy
(~10%), four VU0 programs still interpreted (~5%) and the state flush (2.3%) were larger and each could be made
native or fixed with an exact check against the original. Those went first. Native guest functions follow the rules
already used for VU0 programs: bit- / byte-exact against the original in a verify mode, a switch back, and the
original runs whenever the native version would have to call other guest code (the flush's plug-in callbacks).
Draw-call replay stays the way to remove the remaining copy and flush together.

## 2026-10-24 - Block validity by write watch, not by reading the bytes

Step 4 (FULLY_NATIVE.md) needs to know at the frame kick whether a world / model block still holds the bytes its
recording was made from, without copying or hashing it (the distinct blocks are ~85% of the marked bytes: a per-frame
check by content would cost as much as the copy). Chosen: the OS's write watch on guest RAM, folded into per-page
generations once per kick. Not page protection: the stream thread reads the disc straight into guest RAM, and a kernel
write into a protected page fails instead of faulting. Not hooks on the game's streaming / instancing functions: every
path that writes packet memory would have to be found and kept up. Exact by construction (any write to any of a
block's pages makes it dirty), checked with a decide-copy-compare mode (2.4M decisions, 0 wrong).

## 2026-10-25 - Draw items: one tag in the chain per draw, the block decided at the kick

FULLY_NATIVE.md step 4 asked for world / model draws that make no packets. Chosen: the draw call becomes an item (its
flush when all CNTs, its GS registers, its block), and the packet buffer keeps one 16-byte CNT per draw whose MARK /
NOP name the item and the frame. Not "no tag at all": 2D, effects, texture uploads and the setters' own uploads still
go through the chain between the draws (step 5), and the tag is what keeps each draw in its place among them. The
block's validity is decided at the kick, after the write watch's fold, exactly as for a marked block - not at the draw
call: generations there only cover writes up to the last kick, and a cell streamed in this frame and drawn this frame
would be replayed from an old recording. A block that changed is copied from guest RAM at the kick (`flattenBlock`,
the walker's rules; checked against the walker's copy), so nothing reads guest RAM later on the worker.
Draws stay packets where an item would not stand for the same bytes later or cannot be made exactly: while
`FUN_00240ED8` caches a sector (the copy is sent again in later frames: game-internals.md 6i), when the flush calls
plug-in callbacks (guest code: ~15% of draws while driving), for a model block not yet seen copied by the walker.
Speed was not the reason (measured even: the walker's share falls by about what the items cost); the reason is the
path - the draws no longer depend on the chain's bytes, which step 5 needs.

## 2026-10-25 - Draws whose flush calls guest code: captured, not ported

Step 5f could have ported everything a state flush may call (FUN_001CFAE0's float maths, the two immediate-geometry
callbacks) so that every draw is built natively. Chosen instead: those draws run the original and the region they
wrote is captured into items (the 5d mechanism, with CALLs taken as blocks). The chain loses their packets all the
same, nothing new has to be checked bit for bit, and the guest code keeps doing what it does. A port stays possible
later if their CPU time ever matters (small: ~2% of flushes).

## 2026-10-26 - Gaps: take what is between hook points, in place

Step 5h could have gone on adding writers to the captured list one by one (the census by origin named them). Chosen:
every hook point takes what was written since the last one, whoever wrote it, so the list never has to be complete.
Taken in place (item tag + NEXT over the rest; the cursor never moves back), because a gap is settled at arbitrary
hook points where a caller may hold the cursor - one such caller (`FUN_00240ED8`) was found the hard way, and verify
mode cannot see this class (it keeps the packets). The price is a NEXT per run (~32 a frame) and dead bytes in the
buffer; the chain's walk in 5g skips both.

## 2026-10-26 - No second way to start a frame (FULLY_NATIVE.md 5g dropped)

5g would have handed the item list to the worker without walking the chain. Measured after 5h: the kick takes 0.23 ms
a chain, 0.19 ms of it the items' resolve, which stays either way; the walk and copy of the ~300 remaining tags (the
root lists, the item tags, the runs' NEXTs) cost ~0.04 ms. The chain is kept as what orders the frame: one path, the
walker's checks (verify modes, the census) keep working, and nothing is drawn from the chain's own bytes any more.

## 2026-10-26 - zlib natively as an exact port, not a faster inflater

`inflate_fast` could be replaced by a faster decoder (wide bit-buffer refills, multi-symbol tables) with the same output.
Chosen: zlib 1.2.2's own loop, so every call leaves `next_in` / `avail_in` / `hold` / `bits` exactly as the guest's -
checkable call for call against the original (`VCS_NATIVE_INFLATE_VERIFY`) - with only the copies that cannot overlap
done as `memcpy`. A faster decoder would stop at other points and leave zlib's state different between calls (the
final output the same); the ~20 ms an area load costs now did not justify losing the per-call check.

## 2026-10-27 - The optimised build is the default; `--quick` for development
The user asked for installing to be as easy as possible. A fresh build directory now configures with ThinLTO
(`PS2X_LTO=THIN`, `--opt lto`), so `python tools/build.py --recompile` from the README gives the game to play;
`--quick` (= `--opt none`) is the fast-relink build for changing code. Not PGO as the default: training plays the
rig's card copies (build/rig/saves), which a new checkout does not have, and takes ~30 min more. The choice stays in
the CMake cache as before (one build directory: switching recompiles the game, and a non-sticky default would make
every quick build followed by a plain one cost a full rebuild); the build's last line names it so nobody plays a quick
build by accident. The build also writes `iso=` into the vcs.ini next to vcs.exe when there is none: the exe is a
windows-subsystem program, so started by a double click without a disc it fails with no visible message.

## 2026-10-27 - Full screen is borderless only; test runs never write vcs.ini

Full screen covers the monitor at the desktop's resolution with no display mode change. With a flip-model swapchain
Windows 10+ presents a window covering the monitor directly, so exclusive mode buys little, while a mode change costs
seconds per switch, breaks Alt+Tab and moves the other monitors' windows; the game's resolution is the Internal
resolution setting, not the monitor's mode. Exclusive full screen can come later as a third choice if someone needs a
lower monitor mode. Hidden test runs read vcs.ini but never rewrite it, so a scripted check (Alt+Enter, the settings
menu) cannot change the player's settings.

## 2026-10-27 - 4:3 screens sit inside the game's frame, not the window

The user's choice: in a ratio wider than the monitor (32:9 on 16:9) the pause menu stays 4:3 but sits in the game's
strip between the bars, at the strip's height, rather than over the whole window. Loading screens follow the same rule
(they are the same kind of frame: no 3D). The intro movies keep the whole window: they play before there is a game
frame to belong to. The menu switches to 4:3 as soon as it opens rather than after the 3D history runs out.

## 2026-10-27 - Ultrawide: fixed ratios plus Match window, Hor+ between 4:3 and 32:9

The user asked for 21:9 and 32:9. "21:9" monitors are 2.37 (2560x1080) or 2.39 (3440x1440), not 21 / 9 = 2.33. Chosen:
- The menu's 21:9 is exactly 21 / 9 (the label says what it is), and a Match window choice takes the window's own
  shape, so every monitor gets a picture without bars; it follows the window each host frame (resizing, full screen).
- Match window is clamped to 4:3 .. 32:9: narrower than 4:3 would cut the original horizontal view (we only do Hor+),
  wider than 32:9 is untested (triple screens would need checking: the view passes 140 degrees).
- No separate FOV setting: the game's horizontal FOV is kept for the middle 4:3 and the sides are added.

## 2026-10-26 - The trainer: the game's script commands, nothing saved, the game's input taken while open

The user asked for a trainer like RDR2's (docs/TRAINER.md). Chosen:
- Everything the game has a script command for goes through it (run on the game's own script VM), rather than
  writing the game's structures: the commands keep the game's own bookkeeping (wanted level and police, streaming,
  vehicle creation and ownership). Only values that drift every frame (health, ammunition, a clip, a vehicle's
  health) are written directly while their switch is on - a command per frame would cost a script each time.
- Switches are not saved in vcs.ini: the game starts authentic every time; the trainer is used, not configured.
- The menu takes the game's input while open (as the settings menu does): its keys are the game's D-pad, Start and
  Select. RDR2's trainer lets the game keep the keys it does not use; here every menu key is a game button.
- Trainer scripts must never be in a save: started only while the game runs, taken back out when it pauses.
- Models are streamed asynchronously with a time-out rather than `LOAD_ALL_MODELS_NOW`, which froze the game when the
  streaming memory was full.

## 2026-10-28 - Music / effects volume: the game's own levels, kept across loads

The user asked for music, sound effects and voices volume, with the music / effects changes shown in the game's own
menu. The game has exactly two levels (its pause menu's Music / SFX Volume), so those two settings *are* them rather
than extra gains on top: one value, two menus. On the PS2 a save carries the levels and a load restores them; here the
setting wins (every non-menu call of the game's setters gets the player's level), because a PC game keeps its settings
across saves and the newest save loads at every boot - the save's levels would otherwise undo a change made without
saving. The levels are percent of the game's 0..127 (linear, as the game's bars are); a pause-menu step is 16, so the
setting shows the nearest percent and the game keeps its exact level.

Voices volume was added the same day as a gain on the speech voices in the native driver (the game has no level for
speech) and **removed on the user's decision**: it felt inconsistent, since only speech played as effect sounds
followed it, while speech in cutscenes and on the radio is mixed into those streams. The code is gone (the driver is
as before); game-internals.md 9c keeps what was learnt.

## 2026-10-28 - Player settings: bindings in the game layer, the mouse as stick speed, defaults as before

The user asked for flexible settings players can change. Choices:
- **Where the bindings live**: in the game layer (src/game/vcs_input.cpp), feeding the runtime's pad backend through a
  reader hook; the runtime keeps its fixed mapping as the reference (`VCS_INPUT_MAP=0`), as the rules want for any
  replaced path. Injected (control-channel) keys go through the bindings too, so every scripted check exercises them.
- **Two slots per action**, any key / mouse button / wheel notch, Shift / Ctrl / Alt without a side (one slot holds
  "Shift" where the old mapping needed two). One key, one control: binding steals it from the other control, and the
  menu keys can neither take nor lose theirs - a silent double binding was judged more confusing than the steal.
- **Mouse camera**: the PS2 game has only a stick camera, so the mouse's speed becomes the right stick's tilt (not its
  distance per frame), which keeps the turn per hand movement the same at any frame rate; the stick's top speed caps
  flicks. True mouse look would mean writing the camera's angles in the game's memory - later, if the stick feel is
  not good enough. Off by default (it holds the cursor in the window).
- **Vibration**: the game's own motor levels (it already shook the pad; the stub dropped them), 100% = as on the PS2.
- **Defaults**: exactly the old fixed mapping and behaviour (keep running in the background, vsync off as the flip
  model presented before), so nothing changes for a player who opens no menu. The Authentic preset is the Display /
  Graphics defaults; presets never touch controls or sound.
- **Volume** squared (the slider's middle is about -12 dB), master after the mix so the game's own levels and the
  SPU2 output stay bit-identical for the checks (`PS2X_AUDIO_DUMP` records before it).

## 2026-10-27 - Per-frame mission meters: each instruction on the time step, not the scripts at 30 Hz

PHI_A2's heat bar (and meters in eleven more missions, game-internals.md 11f) change by a fixed amount every frame,
so they ran 4.8x fast at 144 fps. Two ways to fix it:
- Run the script VM at the PS2's 30 Hz above 30 fps (skip CTheScripts on frames in between, with the time step summed
  for them). Every per-frame script assumption would be as on the PS2 at once, cutscene camera moves and object
  animations included - but everything scripts move each frame (attached objects, cameras pointed per frame, cars
  held to a speed) would update at 30 Hz inside a 144 fps picture, script reactions would come up to 33 ms late, and
  the timers / WAITs would have to be fed the skipped time. Changes the whole game's behaviour for a few missions.
- Chosen: scale just the listed instructions by the frame's step over 1/30 s, the way the game's own timed opcodes
  (0x0031 - 0x0034) do it - what the mission would have used had the PS2 not been locked to 30. Every site is in a
  table with its mission, offset and decoded operands and is checked before it is touched, so a different build or a
  mod's MAIN.SCM leaves it alone; the survey that found them is written down so the next one is a table row. Visual
  per-frame animations are left at the frame rate for now (looks only).
Off in original timing at 30 fps (the game's own handlers, bit-exact); `VCS_SCRIPT_TIMESTEP=0` turns it off elsewhere.

## 2026-10-28 - Per-frame vehicle physics: scaled by the step, the brake's constants from guest RAM

A car running over a body jumped and a stolen bike flipped at high frame rates (game-internals.md 4): the push of a
body under a wheel (FUN_003F34F0) and ProcessWheel's rolling resistance act a fixed amount once a frame.
- Scaled by the frame's step over 1/30 s, as the mission meters (2026-10-27) and reVC's FIX_BUGS
  (`CTimer::GetTimeStepFix()`) do - only when the frame is shorter than the PS2's. Longer frames keep the game's
  amount: the PS2 game itself applied it per frame when it dropped below 30, so 25 fps plays as it did there.
- The push goes through ApplyTurnForce, so its hook scales the one call by return address; no recompile, and
  `VCS_PHYSICS_TIMESTEP=0` gives back the reference.
- The brake is built from immediates in the middle of ProcessWheel, where no hook reaches (rule 2: nothing after the
  original returns). Reimplementing ProcessWheel natively would mean the guest RNG and two guest calls; instead the
  four constants are patched to `lwc1` from guest RAM 0x7FF00.., which the ProcessWheel hooks write (scaled) before
  every call. Unscaled they are the game's exact floats, so 30 fps stays bit-exact. Those 16 bytes below the runtime's
  callback stacks are ours now (game-internals.md 4); the hooks are required by the patched code, not an option.
- CBike's lean `max(step, 0.5)` patched to 0.001 like the ped and cutscene ones (the same class; above 100 fps only),
  then the same in cars / bikes in water and a boat's rudder. Each patch was measured before and after; one that
  changed nothing measurable (the buoyancy's lift cut-off, game-internals.md 4) was taken back out.

## 2026-10-29 - vcs.exe at the top of build/, native by its own defaults (the user's request)

- The user found the exe "buried": `build/release/third_party/PS2Recomp/ps2xRuntime/vcs.exe`. The main build tree
  (build/release) now links it to `build/vcs.exe`, with its DLLs (staged next to the target, so they follow) and
  `vcs.ini` (CMakeLists.txt, `RUNTIME_OUTPUT_DIRECTORY`). Only that tree: PGO training's build/pgo-gen keeps its exe
  in its own folder, so the two never write the same file. build.py moves a player's vcs.ini from the old folder and
  deletes the old exe / pdb, so no stale build is left to start; clean.py never deletes the game files in build/.
- A double-clicked vcs.exe ran the software GS at ~5 fps: only run.py / scenes.py set `PS2X_GS=hw`, so every test
  ran a configuration players never got. The launcher now defaults to hw, and the tools no longer set it - they run
  vcs.exe with its own defaults, as a player does. Every other native path was already on by default (each switch is
  `=0` to go back to the reference): with no PS2X_* / VCS_* set, gameplay shows gs_draws 0, vu1_programs 0, the
  native sound driver, the emulated IOP off and the render worker on.
- The runtime made empty mc0 / mc1 folders in the working directory before loadELF set the card path; the launcher
  now sets the card folder before the runtime starts.

## 2026-10-29 - Mouse look on foot, driving keys (the user's request)

The user asked for the mouse to move the camera while walking, right click to lock on to peds, and W / S / Space as
accelerate / brake / handbrake while driving.
- **Mouse camera on by default** now: the user wants the mouse to steer the camera, and with it off a new player's
  mouse did nothing but click. It is a control binding, not a change to the game's look or rules, so the
  "authentic by default" rule does not hold it back; it stays a toggle (Keyboard > Mouse camera).
- **True mouse look on foot** (the "later" of the 2026-10-28 entry above): the follow camera's heading is written in the
  game at the pad read, and its source points turned with it - the camera otherwise pulls the heading back to where it
  stands (game-internals.md 12b). The pitch cannot be written that way (the camera rebuilds it from where it stands), so
  it goes through the stick's own pitch speed with the game's stick curve undone and the excess over its top speed
  carried to the next frames: the angle is exact, only very fast vertical flicks arrive a little late. Writing the
  pitch directly would mean moving the source points up / down and their collision with the world - not worth it for
  the ~0.1 s it would save on the fastest flicks.
- Every other camera (vehicles, sniper / first person, lock-on) keeps the stick path: each has its own code, and the
  stick is what they were made for. The stick path now aims past the game's dead third of the stick (FUN_002842e8),
  so slow mouse movements turn those cameras too - before, under a third of full speed did nothing.
- Mouse forward now looks up. The old stick path sent it the other way (the game looks up for a stick pulled back) while
  the menu said "forward looks up".
- **Right click** was already R1 (target / lock-on); checked that it locks on with fists and a pistol and that the mouse
  moving meanwhile does not break the lock. Nothing to change but the check.
- **Driving keys** are a section of their own with their own key group: a key may be W on foot and Accelerate in a car,
  which the one-key-one-control rule of the 2026-10-28 entry would have refused. While the player drives (ped +0x480
  set and state 0x37) the driving bindings apply and the on-foot bindings of the same keys do not (W must not also tilt
  the stick: on a bike that leans, in a helicopter it pitches). The menu keys stay exclusive across groups. Lean
  forward / back (bikes, aircraft) moved to Keypad 8 / 5, since W / S no longer do it in a vehicle. The gamepad keeps
  the game's own layout.

## 2026-10-30 - The trainer's free camera (the user's request)

The user asked for a free-cam mode in the trainer, and chose: the world keeps running while it flies (the player
stands), Hide HUD as a switch of its own, on / off from a trainer row and a hotkey (F6, gamepad Back + LB).
- **The camera written after the game's update, not a script camera.** GTA trainers usually move a script camera
  (SET_FIXED_CAMERA_POSITION + POINT_CAMERA_AT_POINT) every frame. Here that would be a script per frame through the
  trainer's queue, a frame late, and only between frames. Instead the free camera writes the active CCam's source /
  front / up / FOV right after the game's own update of it (in place of the height routine CCamera::Process calls
  next, FUN_002A2ED0; game-internals.md 12b); CCamera::Process does the rest, so culling, LODs and streaming follow
  with no extra work. A hook of the next call, not code after the original returns (AGENTS.md rule 2);
  `VCS_FREECAM=0` keeps the game's code untouched.
- **The game's update keeps running** (changed the same day): the first version replaced CCam::Process while flying,
  and a mission launched under it hung on "Loading..." (the user's report) - its cutscene ends only when the cutscene
  camera has played its path, which that update advances. Running it and overwriting afterwards fixes every cutscene,
  wherever the mission was started. Launching a mission from the trainer also turns the free camera off (the user
  was fine with that as the fix): one starts a mission to watch it.
- **Real time**: the camera moves by the host clock, so its speed is the same at 30 / 60 / 144 fps and under the
  trainer's Game speed (slow-motion shots fly normally).
- **Fixed keys** for flying (W / A / S / D, E / Q, mouse; the gamepad's sticks and triggers): the game gets none of the
  input meanwhile, so they cannot clash with its bindings; only the on / off key is a setting. Rebindable flying keys
  can come later if wanted.
- **Hide HUD** skips CHud::Draw (the hook vcs_hud.cpp already had, now installed with `VCS_HUD_RW=0` too): radar, clock,
  money, weapon, stars. Text drawn after it (zone names, help, subtitles) stays - hiding that too would mean picking
  which text is the HUD's.

## 2026-10-30 - Native Linux (the user's request); the Steam Deck postponed

The user asked for Linux to be as native as Windows, first with Steam Deck support, then (the same day) without it:
"just have strong support for linux for now". Their choices: a **plain host build** (the distro's own compiler and
libraries through the same `tools/build.py`; no container, Flatpak or Steam Runtime), tested on their Linux laptop
(i5 Skylake, HD 520, 7 GB) with the Windows PC kept as the reference. The working list is docs/LINUX.md.
- **clang only** (19+, for `preserve_none` in PS2_GUEST_CC): the same compiler as clang-cl, so the same code and
  numerics. GCC would need its own checks of every bit-exact path and lacks preserve_none.
- **Flags as clang-cl's**, written out: `-march=x86-64-v3 -ffp-contract=off -fno-math-errno`, and `-fno-strict-aliasing`,
  which clang-cl implies (MSVC compatibility) and plain clang does not - the runtime and generated code read guest memory
  through casts. No stack protector / CET, as `/GS-`.
- **Vulkan**, a new RHI backend (`src/rhi/vulkan`), rather than OpenGL or a translation layer: the RHI was shaped for it
  (immutable pipelines, explicit passes), and it can be checked against D3D11 on the same Windows PC
  (`-DVCS_RHI_VULKAN=ON`). D3D11 stays the Windows default and the reference. Vulkan 1.3 with dynamic rendering,
  synchronization2 and push descriptors; volk, VMA and the headers from source (no SDK).
- **Shaders stay one HLSL source**: glslang (a distro package) compiles them to SPIR-V through `tools/hlsl_spirv.py`.
  DXC was the plan, but no distro ships it here and a downloaded binary is a tool the user didn't install; glslang
  handles every shader, and the script fixes the one thing it gets wrong (dual-source output: Location 0 Index 1).
- **Dear ImGui through the RHI** (`render/ui_renderer.cpp`) for Vulkan instead of `imgui_impl_vulkan`: one pipeline
  works for every target format the backend draws to (the swapchain's BGRA and the RGBA captures), and it uses only
  the RHI. D3D11 keeps `imgui_impl_dx11` (the reference stays as it was).
- **Hidden test runs make no swapchain** on Vulkan: the back buffer is a texture. A FIFO present to a hidden Wayland
  window can wait forever, and tests take screenshots off-screen anyway.
- **A null backend** (`VCS_RHI=null`) to bring up and check the CPU side without the renderer.
- The Windows-only debug tools got Linux versions where cheap (crash handler on signals, a SIGPROF sampling profiler);
  `perf` / `gdb` cover the rest. The write watch has no Linux equivalent yet (correct without it, slower); whether it is
  worth userfaultfd is to be measured first.

## 2026-10-30 - Frame-rate independent timing shows every frame it renders

The user asked for more performance on both platforms and chose this first (with specialised GS pixel shaders next).
With frame-rate independent timing the game thread now queues one frame and the window shows every frame rendered
(PERFORMANCE.md 17: 30 -> 76 shown fps on the GPU-bound laptop). Original timing keeps the old pacing on purpose: there
game time is counted in frames, and holding the game thread back would slow the game down when the GPU can't keep up,
where showing fewer frames keeps it at speed.

## 2026-10-30 - Input delay first: host-paced game frames, the frame lead

The user felt more input lag after the show-every-frame change and asked to keep input lag to a minimum. It is measured
now (`PS2X_RHI_STATS`: input snapshot to GPU done) and the frame-rate independent pacing was rebuilt around it
(PERFORMANCE.md 19): the main thread's frames pace the game's, a backlog is drained, and the game's frame starts early
enough (the frame lead) to be ready just in time on input read just before. 60 cap: 50 -> 34 ms. When the GPU is the
limit the one frame waiting stays (dropping it cut 9 ms but cost 4% of the frames and visible judder): smoothness kept
there.

## 2026-10-30 - A launcher for releases (docs/LAUNCHER.md)

The user asked for a launcher, as other recomps have, to be included in releases: locate the game, build, launch;
Windows first; the Python scripts must stay usable on their own.
- **Distribution stays source-only.** A release is the source plus `VCSLauncher.exe`, python.org's embeddable Python
  and `deps/` (the open-source libraries CMake would download) - no game code: the game is still recompiled and built on
  the player's PC. `tools/release.py` checks every file before zipping (no disc images, no MIPS ELF, nothing from
  generated/ or build/) and refuses the release otherwise.
- **The launcher is a front end over `tools/`**, never a second pipeline: it runs `doctor.py`, `build.py`,
  `setup_vs.py`, `backup.py`, `report.py`, `update.py`, `rip_disc.py` and reads their `@vcs {json}` events. A player
  with the launcher and a developer with a terminal run the same code.
- **SDL3 + Dear ImGui** for its window (already dependencies, the look of the in-game menus, portable later), its own
  CMake project so CI builds it without a disc; one static exe.
- **Bundled Python and libraries**: Visual Studio's compiler is the only thing left to install, and the launcher runs
  Microsoft's installer for it (checked for Microsoft's signature first; one UAC prompt). The release builds with no
  git and no internet (`FETCHCONTENT_FULLY_DISCONNECTED`). Cost: an 82 MB zip instead of a few MB.
  From that zip in an empty folder the game built in 8 min 42 s on the dev PC (the README said ~15 min on a laptop).
- **Updates only when asked** (or at start when the player turns it on): the check is one request to GitHub's API.
- The launcher's own settings live in `build/launcher.ini`; the game's stay in `vcs.ini` (F2 in the game).
- Not done: code signing (SmartScreen warns about an unknown publisher; documented in the README).

## 2026-10-30 - 60 fps with frame-rate independent timing by default

The user asked for frame-rate independent timing and 60 fps to be the default for every player from their first
build. `display.timing` defaults to 1 and `display.fps` to 60 (src/main.cpp); everything else stays the PS2's (4:3,
original resolution, no anti-aliasing). This replaces "60 fps off by default" (2026-10-22): the one enhancement on by
default, the rest still opt-in.
- *Authentic (PS2)* sets original timing at 30 explicitly (it used to be "the defaults"); *Reset to defaults* now gives
  60 fps independent timing; the menu's help says so.
- vcs.ini only keeps values that differ from the defaults, so players who never touched the frame rate get 60 fps on
  their next build too; anyone who chose a frame rate keeps it.
- Test runs keep the old timing: hidden runs (toolchain.test_window) pin `VCS_TIMING=0` and `VCS_FPS=30` unless the
  test sets them, so the regression baselines, the bench numbers and the checks mean what they did. controls_check
  passes an empty `VCS_TIMING` (not pinned, ignored by the game) because it tests the presets. The PGO profile is still
  trained at original timing (bench / scenes); retrain at the new default if the gap shows.
- Checked 2026-10-30 in a hidden run: reset -> timing 1, 60; Authentic -> 0, 30; Widescreen, light -> 1, 60; a test
  run -> 0, 30.

## 2026-10-31 - CMake and Ninja from anywhere, else downloaded

A player's build stopped at *Configuring the build* with a traceback: with Visual Studio Build Tools only, cmake is
in Visual Studio's own folders, which the checks looked at and Windows didn't (it starts a child's program from the
launcher's PATH, not the one given to it). The user asked for CMake to be found whatever the setup.
- **Search, then download.** `toolchain.build_tools` takes the first CMake >= 3.21 and Ninja >= 1.10 that run (each
  one is started: `cmake -E capabilities`, `ninja --version`): the build environment's PATH first (what the player
  chose), then every Visual Studio install's bundled copy (vswhere -all and the default folders), Kitware's
  installer's folder, winget, scoop, Chocolatey, pip (this Python and the user's), CLion, Qt, the Android SDK and
  MSYS2's native environments (its POSIX ones refused: msys-2.0.dll / cygwin1.dll beside the program). With none,
  Kitware's / Ninja's own builds are downloaded once into `build/build-tools` - pinned versions (CMake 4.4.5, Ninja
  1.13.2: what the dev machine builds with) and SHA-256 hashes in toolchain.py, so a changed file is refused.
- **Versions are checked, not just names**: VS 2019 bundles CMake 3.20, below the project's 3.21 - found on the dev
  machine itself. A too-old one is skipped with a line saying why.
- **Always by full path**: `resolve` gives cmake its path, `CMAKE_MAKE_PROGRAM` gives CMake Ninja's. The chosen folders
  are not put on PATH, which would let a folder like Chocolatey's or pip's shadow clang-cl. A build tree whose CMake or
  Ninja has gone (`stale_tools`) is set up again with today's.
- `VCS_CMAKE` / `VCS_NINJA` (full paths) choose by hand, checked like the rest. doctor never reports CMake as missing
  any more: it says the build will download it; only a failed download stops a build (`no_cmake`).
- Not bundled into the release zip (55 MB more for the rare player with no CMake and no internet): Visual Studio's
  installer, which the launcher runs, brings CMake anyway.

## 2026-10-31 - Release zips dated a day before the commit; the build repairs future dates

A player in Brazil (UTC-3) couldn't build 0.0.1: Ninja re-ran CMake 100 times and gave up ("manifest 'build.ninja'
still dirty"). Zip dates have no time zone; Python's zipfile writes the packer's local time (Adelaide, UTC+10:30), so
every file came out up to 13.5 h in the future, newer than any build.ninja CMake could write. Both ends are fixed:
- **release.py** dates every file the commit's time minus a day, as UTC (`zip_time`; `SOURCE_DATE_EPOCH` when set):
  in the past in every zone, and one date for the whole zip. Not each file's own time made safe: nothing in the
  build needs those times, and one date makes the zip depend only on its content.
- **build.py / recompile.py** set files dated in the future to now before CMake runs, with a line saying so, rather
  than stopping with a message: the player can't fix the zip, and touching a file's date loses nothing. doctor only
  reports them (it changes nothing). A build tree that already looped recovers without being deleted (checked).
- Ninja's "still dirty" still gets its own failure (`file_dates`) for what the repair can't reach - a clock that is
  wrong, a read-only file.

## 2026-10-31 - The CPU baseline follows the PC that builds (AVX2, AVX, SSE4)
- A player's report (Xeon E5-2687W, Sandy Bridge: AVX but no AVX2): "recompiling MIPS code to C++ fails in 1 second",
  `recomp.log` empty. Every target was built with `/arch:AVX2`, so `ps2_recomp.exe` - and later `vcs.exe` - stopped at
  its first AVX2 / BMI instruction (`STATUS_ILLEGAL_INSTRUCTION`, 0xC000001D) before printing a line; the tools didn't
  say how it ended, and the doctor didn't look at the CPU.
- Players build on their own PC (source-only releases), so the build is compiled for that PC's CPU:
  `PS2X_CPU=AVX2|AVX|SSE4` (`ReleaseMode.cmake`; `/arch:AVX2` / `-march=x86-64-v3`, `/arch:AVX` / `-march=sandybridge`,
  x86-64-v2), chosen by `toolchain.cpu_level()` (Windows `IsProcessorFeaturePresent`, Linux `/proc/cpuinfo`) at every
  configure; a tree set up for another level is configured again (a full rebuild). `VCS_CPU=AVX|SSE4` builds an older
  CPU's game on a newer PC (how it was tested). Below SSE4.2 (Core 2, 2006-08) the build stops with `cpu_unsupported`.
- One binary with runtime dispatch was the alternative: it would need every hot path written twice and picked per call,
  for a build players make themselves anyway. AVX2 stays the default and what releases are measured with.
- Numerics are the same at every level: FP contraction is off everywhere (no FMA anywhere since 2026-10-22), and the
  vector paths (`ps2_vu1_compiled.h`, `__SSE4_1__` in `ps2_runtime_macros.h`) have per-lane fallbacks that do the same
  operations. `vu1v` (compiled VU1, native VU0) needs only AVX, so it is on `__AVX__` now: AVX builds keep it, SSE4
  builds take the per-lane version (which had never been compiled: `vcs_vu0_native.cpp` shuffled `__m128` itself).
  Checked by recompiler output and scene captures (PROGRESS.md, 2026-10-31).
- A tool that fails now ends its log with how it ended (`toolchain.describe_exit`: the NTSTATUS name, or the signal),
  and the launcher gets `illegal_instruction` / `missing_dll` instead of a bare `recompiler_failed`.

## 2026-10-31 - Free aim: the game's own, driven by the host (the user's request)
The user asked for free aim with a reticle, selectable in the settings, that feels as if the game had supported the
mouse all along; their choices: aim while the right button is held, no lock-on with the mouse when it is on, free aim on
the gamepad too (no aim assist), the mouse 1:1 in scopes and the wheel for weapons / zoom; no mouse steering. One rule
from them: Vic moves while free aiming exactly when he can move locked on with that weapon - nothing new is added.
- **The game's free aim, not a new one.** VCS has free aim (R1, then L3; game-internals.md 12c): a lock-on to an
  invisible dummy object with its own circle reticle, aim pose, camera and no random spread. Building on it keeps all of
  that (the reticle is the game's, scaled with the HUD; peds react as to any aim) and replaces only what feels bad: the
  pad read enters it in one report (R1 + L3), the mouse / right stick turn the dummy about Vic (writing it every frame,
  turning the lock-on camera by the same amount first so it does not lag), and the world update runs with the free-aim
  bit cleared, so the game treats it as a lock-on to an object: Vic moves as he does locked on with that gun (the game's
  free aim makes him stand still and puts the left stick on the aim). The camera and the HUD run with the bit set again
  (the centred free-aim camera and the circle, not the lock-on view and triangles). The dummy stays 12 m from Vic: the
  lock drops past ~30.
- **Shots through the circle**: the gun's line to the dummy and the camera's line through it cross only at the dummy,
  so near targets would be missed by the gun's line. The player's free-aim shot tests the camera's line instead (the
  line test's start redirected to a point of it level with the gun; GTA VC's PC version does the same with its
  third-person mouse aim), and the shot's end is put on it so the tracer agrees. The gun's own start stays for the
  muzzle effects.
- **Not an ImGui reticle** (the plan's first idea): the game draws its own circle exactly where the shots go, in the
  HUD's style and scale; a second one would only disagree with it.
- **Melee, thrown and scoped weapons keep R1** (lock-on is how fists fight; scopes are already free aim). Off by default
  for both devices: it changes how the game plays (easier than lock-on at range), so "authentic by default" holds it
  back, unlike the mouse camera. Mouse wheel is on by default: a binding, nothing in the game changes.
- Hip fire (the left button with a gun) also aims for the shot, held 0.6 s after it, so every shot goes through the
  circle as in a PC shooter.
- `VCS_FREEAIM=0` installs none of the hooks (the reference). The guest scratch for the shot's start sits at 0x60000 in
  the EE kernel's area, which nothing in the recompiled game uses.
- (later the same day, the user) Going into and out of free aim was too slow: the game blends the camera 400 ms in and
  350 ms out. Both blends are cut to 100 ms, only those two (the frames after free aim starts or ends): not an
  instant cut, which jumps the view by about 2 m, and the game's other blends (vehicles, cutscenes, the PS2
  lock-on) stay as they are.
- (the user, after that) A lag between letting go of the right button and the aim ending: the hip-fire hold (0.6 s
  after a left click) also held aims fired with the right button down - 15-17 frames after a shot. The hold is now
  for shots fired without the target button only, and letting go of it clears the hold: 2 frames from the release to
  the camera leaving the aim, shots or not (measured with the rig's `trace`).
- (the user, next) Leaving the aim felt quicker than entering it - the camera blends were the same (100 ms, measured),
  but entering also has Vic raising the gun. The exit blend is 200 ms now (the user's choice). And on release the view
  snapped to another angle for one frame: the follow camera's first update resets its heading; that frame's view is now
  turned back to the aim's heading in the hook after CCam::Process, before the frame's camera is built (the pad read's
  hand-back came a frame late). The hand-back needs the free camera's hook (VCS_FREECAM=0 leaves it out).
- (the user, next) With guns that stand Vic still the aim's end was instant, unlike the walking guns. The game does that
  on purpose: CCamera::CamControl jump-cuts from the lock-on camera to the follow camera when the weapon can't move
  while aiming (FUN_001F1698). Free aim answers that one question (its call in CamControl, only in the frames after the
  aim ends) with yes, so all guns end in the same blend; Vic still stands still with those guns while aiming, and the
  PS2 lock-on (free aim off) keeps its cut.
- (the user, next) A gun running dry in free aim switched to the lock-on and flicked in and out. The game puts the
  fists in Vic's hand when the gun is empty; free aim then let R1 through (fists lock on, as on the PS2), and retrying
  the free-aim entry (R1 + L3) while Vic was busy could leave the game a frame of R1 alone - a lock-on to a ped - before
  giving up and trying again. Now: once a free aim has ended because the gun went, R1 stays up until the button is let
  go (a gun back in hand, e.g. by the wheel, aims again); an entry the game answers with a ped lock is let go at once,
  and every retry waits 8 frames with R1 up. Starting with fists or melee is the PS2's R1 as before.
- (2026-11-01, the user) Jive Drive's shooting from the passenger seat still aimed with the left stick. That is the
  game's own free aim, which the mission starts itself (SET_PLAYER_DRIVEBY_MODE; game-internals.md 12c) - the only
  mission that does. With free aim on for either device the host turns its dummy in the car too: the mouse and the
  right stick, no button (the game aims all the time there), the left stick centred so the two don't fight. Kept from
  the game: the dummy at the gun's range, the world direction while the car turns, 20 deg up / 5 down; the shots go
  down the camera line as on foot. Both devices' sticks / mice turn it whichever setting is on: there is no button to
  tell the devices apart, and the left stick no longer aims. Free aim off is the PS2's left stick.
- (2026-11-01, the user, next) With the right stick aiming in Jive Drive's car, Circle (fire) was hard to reach. In
  that car aim only, the gamepad's R1 button fires: it is sent as Circle, and not as R1 (the keyboard's R1 keys are
  left as they are). Nowhere else changes: on foot R1 is the target button, and in the car
  with free aim off it is R1 (controls_check checks both).
- (2026-11-01, the user, next) Phil's shooting range also used the left-stick aim, and broke when the right button was
  pressed: the range forces the game's free aim on foot (SET_PLAYER_FORCE_FREE_AIMING, bit 53), and our own free aim's
  entry (R1 + L3) and exit (R1 let go) ended it - Vic back on the follow camera with no circle for the rest of the
  round. A free aim a script forces is now taken over like the car's (one path, `scripted` in vcs_aim.cpp): the mouse /
  right stick, no button, the left stick centred (Vic stands still there anyway), R1 left to the game (it does nothing
  there). The game's on-foot limits are kept: 110 deg either side of Vic's heading, 40 up, 20 down (the game applies
  them only while its own stick moves the dummy, so the host clamps). Asked, the user chose the gamepad's R1 firing
  there too (the same reach problem as the car): it fires in any free aim a script starts, and is R1 everywhere else.

## 2026-11-01 - The launcher draws itself (docs/LAUNCHER.md)

The user asked for the launcher to become a high-performance application with its own native UI and beautiful
animation - "not a sloppy web application", a purpose-built PC utility - multi-platform in design, Windows / D3D11 for
now, built the way uppies (D:\aislop\uppies) is. The user approved the plan and chose: rethink the flow, a custom title
bar, disc art behind Home later.

- **Own window, renderer and UI, no third-party code.** SDL3 + Dear ImGui go: ImGui's look is a tool panel, its
  SDL_Renderer path polled while idle, and SDL made the launcher an 81 s build. uppies' design instead: a platform layer
  (Win32), one instanced draw of SDF primitives (D3D11), immediate-mode widgets with retained animation state. 1 MB, 17 s.
- **Not the game's RHI.** It links ImGui and serves the game's presenter; the launcher wants exact control of
  presentation (latency waitable, tearing, device loss) and must stay a project CI builds with nothing downloaded. Its
  own five-call backend interface takes the same draw list on another API later (Vulkan on Linux), as uppies plans.
- **DirectWrite, not FreeType** (uppies' choice): nothing to bundle or license, Windows' own rasterisation. The atlas,
  cache and layout are platform independent; FreeType is the Linux rasteriser.
- **Three modes instead of pages**: Setup (a guided sequence: one step at a time, the rail filling in), Home (one big
  button over the sunset), Task (the running tool takes the window). Saves / Settings / Help are sheets. Every feature
  of the 2026-10-30 launcher kept its place (the parity list in the plan; LAUNCHER.md "The player's flow").
- **Custom chrome on a normal window**: WM_NCCALCSIZE / WM_NCHITTEST rather than a borderless popup, so snapping, the
  system menu, the min / max animations and the shadow stay Windows'.
- **Zero cost when idle**: the frame loop sleeps in the OS; tool output and process exit wake it (Process's reader
  thread and a thread-pool wait); clocks on screen ask for a frame once a second; nothing animates continuously except
  while a task runs (spinners, the bar's sheen) - never while the game plays (the game wants the GPU).
- **Motion is optional**: the launcher's own *Reduce motion* makes everything snap. (Later the same day, the user:
  panels opened instantly with it off. Windows' "Animate controls and elements inside windows" was off on the PC and
  also turned motion off. Now the launcher's setting alone decides; Windows' only gives its value until it is set.
  Then the user found reduce motion on after updating a release folder, whose launcher.ini didn't have the setting yet:
  that starting value was Windows' again. It is now simply off until the player turns it on, Windows' ignored.)
- **Tests stay hidden**: `--screenshot` renders offscreen on a fixed clock (same arguments, same picture), with
  `--frames` for transitions; `--hidden --quit-after` runs the real window loop unshown and reports its cost.

## 2026-11-01 - The launcher in light and dark, hot pink, no purple

The user asked for a light and a dark mode following Windows' (with System / Light / Dark in Settings), hot pink as
the primary colour - pink and white in light, pink and "a really nice black" in dark - no purple anywhere, other
colours sparingly. The dusk-violet palette and the sunset orange went: dark is neutral blacks and greys
(`#0A0A0B` behind, `#17171A` cards) with hot pink `#FF2E8B`; light is blush white behind (`#FFF4F8`), white cards,
warm greys for text and a deeper, slightly redder pink `#EC1A62` (the user's choice; white text on it stays readable). Teal stays for done, amber and red
for attention - signals only; decoration (the backdrop's sea lines, the playing indicator, the saves icon) is pink.
Colours are fields of a palette struct, so the two blend field by field: switching crossfades instead of jumping.
Overlays (hover tints, hairlines) are `theme::ink(a)` - white on dark, a deep pink-brown on light - and shadows
`theme::shade(a)`, softer and pinker on light. Light mode's scrim washes the content out (a translucent blush)
instead of darkening it. Screenshots default to dark (`--theme`), so docs and tests give the same picture on any PC.
The app icon was redrawn in the same colours.

## 2026-11-01 - Home's backdrop moves (the one exception to an idle launcher)

The user wanted Home's backdrop more sophisticated and "more alive". It became Vice City at dusk - the striped sun
behind palms and art deco towers, a sea that reflects them, stars at night - and it moves continuously but slowly:
stripes sliding into the horizon, clouds, swaying palms, windows, a beacon, rolling water lines, a wavering
reflection. A launcher that draws nothing when idle was a design rule (docs/LAUNCHER.md); this breaks it on Home
only, within bounds: 30 frames a second (motion this slow needs no more; 15 when the window isn't focused), never
while the game runs (it wants the GPU), never with reduce motion on (the scene stands still), never on the other
screens, never minimized. Cost: ~0.16 ms of CPU a frame, about 0.5% of a core. All with the renderer's primitives
(palms are chains of quads, each overlapping the next by 0.7 px so antialiased edges leave no seams; the reflection is
drawn opaque, sunk toward the sea's colour, so overlapping pieces don't show darker).

Next (the user): the palms went, the far row got windows of its own (smaller and dimmer), and no tower has a rounded
top any more (a parapet instead). Then jets (the user): a speck crossing high up every ~30 s - each flight's height,
slant, direction and pace from its number, so nothing is stored - laying a trail of soft dots that starts thin behind
it and widens, sinks and fades over 32 s; white on the light sky (stronger there, or it vanishes), sunlit pink at night
with a blinking strobe. They first all seemed to come from the right (the user): a flight's traits were hashes of
neighbouring numbers, which came out alike. Each trait is now a hash of the flight and the trait mixed apart; slots
(24 s, a random moment in each, some left empty) pair up flying opposite ways in a random order, and each flight also
has its own distance (a far one smaller, fainter, slower), trail life (22-40 s) and patchiness.

Then (the user) the sun's slices looked choppy, and the streaks across it drifted off and never came back. The slices
are the disc drawn through clip rectangles, and the renderer clipped by shrinking each quad to the clip: a clip edge at
a fraction of a pixel snapped to whole pixels, so slices sliding a few pixels a second stepped. The vertex shader now
takes the clip out to whole pixels and the pixel shader multiplies by the part of the pixel inside it - a moving clip
edge is antialiased (checked: an edge pixel's value now changes a little every frame), and a clip on pixel edges, all
the UI's, draws exactly as before. The slices' top edge sits on a whole pixel, where the disc's solid top meets the
first slice, so the two never leave a half-covered seam. The streaks are now scheduled like the jets: one every ~10 s
at a random place around the sun, fading in, drifting right for 35-55 s and fading out.

Then (the user) the jets' starting points and angles more random and varied. A flight's line is now a random point of
the upper sky (10-90% across, 8-32% down) and an angle up to 24 degrees either way; it enters and leaves where that
line meets the window's edges (a little outside them) or the skyline, so jets come in from the left, the right or down
from the top, climbing or descending, and its pace is in pixels a second (a short diagonal is over sooner). Trails
crossing the words on the left are dimmed to 30%, and fade out under the title bar, so a jet from the top seems to come
out from under it.

Two frame-loop faults turned up on the way: a sleep that ended on its timeout set `needsRedraw`, which also books a
settle frame, so every timed wake drew two frames (a timed wake now draws one); and minimized, a frame asked for at a
time already past made the loop wake at once, again and again - a core spinning behind the game while the "Playing"
clock was due (minimized, those requests now wait for the restore).

## 2026-11-01 - Pricedown in the launcher only when the player has it installed

The user asked for Pricedown (GTA's lettering) on the launcher's prominent elements, from a zip of Typodermic's free
desktop release (`Pricedown Bl.otf`). Its licence (Typodermic Desktop EULA v260817, in the zip) forbids distributing the
font (4.1), bundling or embedding it in a desktop application or other software (4.3) and including it in open-source /
publicly available software (4.6); the zip's guide says apps and software need another licence. So the font is not in
the repository or the exe: the launcher asks DirectWrite for an installed "Pricedown" (each player may install their own
copy, 2.1) and falls back to Bahnschrift. Used for three things only: the name on Home, Home's big button, the build
percentage (GTA's money counter is Pricedown). A pre-rendered image of the lettering (fixed artwork, 2.2) was not
chosen: digits for the percentage would be a reusable glyph set (4.5). A licence for embedding would change this.

## 2026-11-01 - vcs.exe runs with the UTF-8 code page (non-ASCII folders)

A player's report (issue #27): the built game stopped at start with "No mapping for the Unicode character exists in the
target multi-byte code page". Reproduced with the game in `build\run\nonascii\Игры é\game`: the folder comes from
`GetModuleFileNameW` (wide), and the first `std::filesystem::path::string()` on it throws, because MSVC's STL converts
to the system's ANSI code page (cp1252 here), which has no Cyrillic. A second case fails the other way: a data folder
with Cyrillic letters (a Windows user name, so `%LOCALAPPDATA%`) arrives through `getenv` as `?`s, and the game "cannot
write" its cache. The game and the runtime pass paths as narrow strings in many places (`loadELF(std::string)`, fopen,
the ini, the card), so converting each to wide calls was not the fix.

vcs.exe now has its own manifest (`src/vcs.manifest` through `src/vcs.rc`, the linker's default off with
`/MANIFEST:NO`): the same `asInvoker` as lld-link's default plus `activeCodePage` UTF-8 (Windows 10 1903 and later), so
the ANSI code page of the process is UTF-8 - `path::string()`, `getenv`, `argv` and the CRT's narrow file functions
all speak UTF-8 and round-trip any name. A resource, not `/MANIFESTINPUT`: lld-link merges manifests only when LLVM
was built with libxml2 (else it calls mt.exe), which a player's clang may not be. Older Windows 10 ignores the setting
and behaves as before.

## 2026-11-01 - The launcher on Linux: native X11 and Wayland, Vulkan, FreeType (docs/LAUNCHER.md "Linux")

The user asked for the launcher to support Linux as well - native, fast, high performance, drawing with Vulkan - and
approved the plan with three choices: **both window systems natively** (picked at startup), **our own icons on both
OSes**, and **CI on Linux now, a Linux release later**.

- **Same launcher, other files.** The launcher was layered for this (2026-11-01 above): `screens/`, `ui/`, the draw
  list, the glyph atlas and the model are shared; Linux adds `platform/platform_linux.cpp` + `platform/linux/` (X11,
  Wayland, D-Bus), `render/vulkan/`, `render/font_freetype.cpp`, `model/process_posix.cpp`, `model/system_linux.cpp`.
  The shared interfaces became UTF-8 (`std::string` argv, `const char *` titles and font names); only the Windows
  files widen. One `ui.hlsl` for both renderers: glslang compiles it (`tools/hlsl_spirv.py --target-env vulkan1.1`),
  its frame constants are push constants under `VCS_SPIRV`, and a flipped viewport keeps its D3D clip space.
- **No toolkit, no SDL** (the 2026-11-01 reasons stand: idle cost, build time, control). Xlib and libwayland-client
  directly, each with what makes it feel native: `_NET_WM_MOVERESIZE` / `xdg_toplevel.move` / `resize` so the window
  manager moves, snaps and tiles; fractional-scale + viewporter (exact scale); cursor-shape (the compositor's theme);
  XDND and `wl_data_device` for dropped discs; XSETTINGS and the portal for dark / light, animations, double-click.
  Wayland is chosen when `WAYLAND_DISPLAY` is set, X11 otherwise (`VCS_LAUNCHER_VIDEO=x11|wayland`).
- **Vulkan opened at run time** (`dlopen("libvulkan.so.1")`, our own table of entry points): a PC without a Vulkan
  driver gets a message instead of a launcher that doesn't start, and nothing links against the loader. Vulkan 1.1 is
  the floor (old GPUs, VMs; lavapipe in CI). Integrated GPUs are preferred: a launcher shouldn't wake a laptop's dGPU.
  Present wait (`VK_KHR_present_wait`) gives DXGI's one-frame latency where the driver has it.
- **Our own icons, both OSes**: Linux has no Segoe Fluent Icons, and bundling an icon font would be a third-party asset.
  `render/icons.cpp` draws each icon from signed-distance shapes on a 24-unit grid into the glyph atlas (one glyph to
  draw, overlapping strokes never darker, 1-pixel strokes snapped to pixel centres at small sizes). Windows' icons
  change with it (same design language, slightly different drawings).
- **System libraries only**: FreeType + fontconfig, libdbus-1, Xlib / Xcursor / Xrandr, libwayland-client / -cursor,
  xkbcommon - every desktop has them, so the launcher stays "no third-party code" in the repository and builds offline.
- **File dialogs through the desktop portal** (the desktop's own dialog, right on Wayland and in sandboxes), else
  zenity, else kdialog. **Packages through `pkexec`** (`build.py --install-deps`: the desktop's own password prompt, the
  logic in Python per AGENTS.md); without a polkit agent the launcher opens a terminal with the same command.
- **Hidden runs on Linux draw offscreen**: presenting to a window that is never shown can block forever under Wayland
  (FIFO waits for a frame callback); the frame loop is paced at the display's rate instead.
- Update checks and copying a DVD are hidden on Linux until there are Linux releases and a Linux `rip_disc.py`.

## 2026-11-01 - Card names the host can't store are %XX; the card is zipped before every change

Players lost saves after saving (game-internals.md 10f): VCS names a slot file after the last mission, and "1 Kill Phil:
Part 2" became an NTFS stream of an empty file after the game had deleted the old slot. And the game deletes a slot
before writing it, and its whole save folder after a failed write, so any crash or host error mid-save loses saves.

- **Names**: the characters Windows refuses (`<>:"/\|?*`), control bytes, bytes >= 0x7F (not UTF-8: they would throw
  under the UTF-8 code page), `%` itself, trailing dots / spaces and device names (CON, COM1...) are stored as `%XX`;
  the game sees the name it wrote. The same encoding on Linux, so a card folder copied between systems keeps working;
  a name Linux stored before is still found. Readable in Explorer ("1 Kill Phil%3A Part 2"), ASCII, reversible. The
  alternative - Unicode look-alikes (U+FF1A for `:`) - reads nicer but is a guess per character and not ASCII.
- **Recovery**: on Windows the card layer turns an empty file whose stream holds 900+ bytes back into the save it was
  meant to be, at the next listing (smaller streams are Windows' own, e.g. a download's Zone.Identifier). Players who
  lost a save this way get it back by starting the new build. If they have saved in that slot since (a new game), the
  recovered save goes to the first free slot rather than hide behind the newer one; the new save is never touched
  (recovery never writes over an existing file).
- **Backups**: before the first delete / write to an existing file / rename / format in 10 s, both cards go into
  `<data>/backups/auto-<date>.zip`, tools/backup.py's own format, so the launcher's Saves sheet lists them and Restore
  works unchanged; newest 10 kept (a card is ~0.5-1 MB, stored). In vcs.exe, not the launcher: players start vcs.exe on
  its own too, and only the card layer knows when a change comes. Saving into an empty slot changes nothing old and
  makes none. `PS2X_MC_BACKUP=0` turns it off.

## 2026-11-02 - Stutter work: the music stop no longer holds the level swap; textures made off the main thread

The user asked for stutters first (issues #16, #29), the game to behave as the original (PERFORMANCE.md 21).

- **The level swap's music stop happens at once.** Crossing between Beach and Mainland the game stops the radio stream
  and waits until it says stopped; CDSTREAM's music thread only sees the stop at its next refill IRQ, up to 448 ms - the
  PS2 stood just as long (game-internals.md 9a). Only that wait changes: the commands, their order and every reply the
  game reads are the IOP's; the stream just ends that much sooner (it is being stopped anyway, and the swap restarts
  the radio after the load - the stations it picks are the same as with the wait). Kept as `VCS_MUSIC_STOP_WAIT=1`;
  the sound replay comparison keeps the IRQ timing, since it checks the driver against recorded IOP times.
- **Textures are made by helper threads on D3D11** and, with anisotropic filtering, their mip chains built on the CPU:
  what the GPU gets is the same (static reference scenes identical), only who does the work and when. Vulkan keeps
  creation on the main thread: its first contents go into the frame's set-up commands, not thread-safe.
- **The stutter rig flies over the roofs** rather than drives the roads: the AI drivers the rig can make stood more
  than they drove, and the route has to be the same every run to compare builds. Streaming, texture bursts and level
  swaps come from the camera crossing new ground at speed, which the flight does.

## 2026-11-02 - Linux: Vulkan makes textures off the main thread when asked, not by default

The user asked whether the Windows stutter work helps the Linux port (PERFORMANCE.md 25). Everything but the texture
helper threads is portable code and was already in. Vulkan now supports free-threaded creation (staged first
contents, recorded at the next submit), but it stays off by default there: Vulkan's main thread only records a copy
per new texture (~8 ms a 350-texture burst; D3D11's creation was 52 ms), and on the 2-core laptop the two helpers
slowed the GS stage's decode more than they saved. `PS2X_HW_EARLY_TEXTURES=1` turns it on; a 4+ core Linux machine
should decide the default. The cruise takes a copy of the player's card (`build/rig/saves/user`) where the rig's
Fort Baxter card is missing; never the live card, and no new game (its first mission keeps the player out of the car).

## 2026-11-02 - Vulkan drops the GS shader's depth output; D3D11 keeps it until checked

The user's city hitches on the Linux laptop are partly the GPU (PERFORMANCE.md 26). The GS pixel shader's depth output
dates from the GL renderer, where NDC lost precision; with D3D / Vulkan's 0..1 depth the rasterised value is the same,
and dropping the output brings early depth rejection back (~1.5 ms of a ~10.7 ms GPU frame on the HD 520). Reference
scenes on Vulkan show no change beyond run-to-run noise. D3D11 is the reference renderer and its GPU has the headroom,
so it keeps the shader depth until a scenes compare on Windows says the same; then both can share the default.

## 2026-11-03 - Linux releases: an AppImage, glibc 2.35 and up, shipped with Windows

For the Linux launcher's first release the user chose **an AppImage** (not a tarball), **support for older
distributions**, and **Linux released at the same time as Windows**.

- **One AppImage, the release inside.** `tools/release.py --platform linux` makes `VCS-Recomp-<v>-x86_64.AppImage`:
  the launcher, its desktop entry and icon, and the release's files (the source, the scripts, `deps/` - no Python or
  FFmpeg: the system's) in `usr/share/vcs-recomp/release`. An AppImage is a read-only image, and the game is built
  inside its release folder (`generated/`, `build/`), so the launcher copies those files to a folder of its own at
  start - `$XDG_DATA_HOME/VCSRecomp/install` (`VCS_INSTALL_DIR`), next to the saves - when it is missing or holds
  another version (`tools/update.py install`). Only files whose bytes differ are written: after an update, Ninja
  rebuilds only what changed. The type 2 runtime is static (no libfuse2 on the player's system).
- **Updates replace the AppImage file** (`$APPIMAGE`, set by the runtime): `update.py apply` downloads the new one,
  checks its SHA-256 and the AppImage magic, and renames it over the old; the next start puts the files in place. No
  AppImageUpdate / zsync: the same update.py and the same GitHub release as Windows.
- **glibc 2.35 is the floor** (Ubuntu 22.04, Debian 12, Mint 21 and newer): CI builds the Linux launcher on
  ubuntu-22.04 with the C++ runtime linked in (`-static-libstdc++`), and release.py refuses a binary that needs a newer
  glibc (`--allow-new-glibc` for a test). 22.04's wayland-protocols is too old for fractional-scale and cursor-shape:
  CI unpacks a pinned 1.45 (`VCS_WAYLAND_PROTOCOLS_DIR`); 22.04's wayland-scanner reads it. The launcher builds with
  any clang or GCC (`toolchain.launcher_compiler_defines`), not only the game's clang 19.
- **The game on those older distributions**: they ship no clang 19, so the package step adds LLVM's own repository
  (apt.llvm.org, as it documents: its key - fingerprint checked when gpg is there - in /etc/apt/keyrings, a sources
  entry for `llvm-toolchain-<codename>-20`) before installing clang-20 / lld-20 / llvm-20. That root work is
  `tools/linux_packages.py`, standalone and short (it runs as root through pkexec or sudo); build.py and doctor.py
  only decide what it should do.
- **Shipped together**: on a `v*` tag each platform's job makes its file and uploads it; `publish` needs both and puts
  them on one GitHub release. A failing platform publishes nothing.
- Tools pinned by version and SHA-256 (no "continuous" builds): appimagetool 1.9.1, the type 2 runtime 20251108,
  wayland-protocols 1.45.

## 2026-11-03 - The mouse on the camera behind a car: free while it moves, then back (the user's request)

The user found the mouse camera in a car slow and sluggish, and asked for it to move freely while the mouse moves and
only a short time after it stops to go slowly back behind the car.
- **Turned by the host, not through the stick.** The stick there has a top speed, is smoothed, and the camera turns
  back toward where the car drives and is dragged after it (game-internals.md 12b, the camera behind a vehicle): the
  mouse fought all of that. A hook before the game's update (FUN_0029FAF0) turns the camera by the mouse's angle, as
  on foot. The gamepad keeps the game's own behaviour.
- **While the mouse moves, the camera keeps its direction in the world**, the car turning under it - nothing drags or
  pulls it (the source points moved with the car, the pull's cap set to 0 meanwhile). Kept relative to the car instead,
  a look to the side would swing with every steering correction.
- **The way back is the host's, at one speed**: an ease to behind the car (1 - e^(-t / 0.6 s), at most 2 rad/s; about 2
  s from a quarter turn) with the drag and pull still off, so it is as slow driving as standing - with the game's own
  camera working during the return, driving brought it back in 0.4 s, standing never. "Behind" is the game's rule
  (opposite to where the car moves, its nose when slow or a boat), so the hand-back is seamless.
- **Car camera return** (Keyboard > Mouse): 1.5 s by default, 0.5 / 1 / 2 / 3 / 5 s or Never. It is part of the mouse
  camera (a control choice, on by default with it); `VCS_CARCAM=0` is the switch back to the stick path.

## 2026-11-03 - Windows without Visual Studio: llvm-mingw as an option, D3DCompile instead of fxc (the user's request)

The user asked for Visual Studio to stop being a build dependency, the way Linux builds with a plain clang. Done as an
option first (the user's choice: keep the Visual Studio build until the new one is checked against it):
- **llvm-mingw** (github.com/mstorsjo/llvm-mingw): one zip with clang, lld, libc++, compiler-rt, llvm-windres and
  mingw-w64's Windows headers and import libraries - D3D11, DXGI, DirectWrite, dbghelp, `wrl/client.h` included.
  `toolchain.py` pins it (20261020, clang 23.1.4, UCRT; SHA-256 from GitHub's asset digest) and downloads it into
  `build/build-tools` like CMake and Ninja: nothing to install, no elevation. `VCS_TOOLCHAIN=mingw` selects it,
  `VCS_LLVM_MINGW=<folder>` takes another copy. Chosen over clang-cl with Microsoft's CRT / SDK fetched from Microsoft
  (the `xwin` way): open source, redistributable, and the same GNU-driver build as Linux, at the cost of a different
  C++ library and ABI - which the checks below cover.
- **It takes CMake's MinGW paths**, the ones Linux already uses (`Clang AND NOT MSVC` in ReleaseMode.cmake: the same
  flags and numerics as clang-cl), plus: `_WIN32_WINNT=0x0A00` (mingw-w64 defaults to Windows 7), `-static` for every
  program (libc++ / libunwind / winpthread inside: only UCRT left, and no Visual C++ redistributable either - the MSVC
  build's vcs.exe needs MSVCP140 / VCRUNTIME140), CodeView in a PDB (`-gcodeview`, `-Wl,--pdb=`) so dbghelp reads it
  as before (the profiler's names and lines checked; the crash handler makes the same calls, not crashed on purpose), and `-fms-extensions` for the profiler's `__try`.
- **Shaders from D3DCompile, for both compilers**: `tools/hlsl_dxbc.py` calls Windows' own `d3dcompiler_47.dll`
  (System32, every Windows 10 / 11) with fxc's flags; the DXBC is byte for byte fxc's (checked for the launcher's
  shaders and the GS / present ones; `-DVCS_DXBC_CHECK_FXC=<fxc.exe>` checks every shader in a build). So the Windows
  SDK's fxc is no longer needed by either build.
- **What it needed in the source**: the VU1 interpreter's exact FMAC check in `double`, not `long double` (MSVC's is
  `double`; mingw's 80-bit one could decide otherwise; PS2RECOMP_PATCHES.md), the profiler's leaf stack read out of
  line (clang's `__try` catches faults in calls only), the launcher's version text from a header (llvm-windres loses
  a quoted define's quotes), and dbghelp names demangled (`src/debug/win_symbols.h`: generated code has only its
  Itanium public symbol, so the profiler counted 0% guest code). Everything else compiled as it was.
- **Checked** (both builds of one commit): launcher screenshots pixel-identical, self-test ok; scenes the same
  (statics mean difference 0.00-0.02); `PS2X_HLE_VERIFY` / `PS2X_VU0_VERIFY` 150 s the same classes (VU0 0 of millions
  differed; the world fast path's few off packets are on clang-cl's build too); SPU2 tests pass; profiler names and
  lines; ThinLTO builds; the Visual Studio build and launcher still build with every change.
- **First not the default, because of speed**: the optimised (ThinLTO) game thread was 5-28% slower by spot, +14%
  driving (PERFORMANCE.md 29). Performance is a hard requirement (2026-10-21), so the user asked for the gap to be
  closed first. It was libc++ on Windows, not the code generation: timed waits that spin their last millisecond, a slow
  `steady_clock::now` and a CRITICAL_SECTION `recursive_mutex`; `runtime/ps2_host_std.h` replaces the three on that
  platform only. Then the game thread was level (-2% to +4% by spot, inside the spread) and the worker / GS stage within
  ~5% while driving, and **llvm-mingw became the default** (`toolchain.WINDOWS_TOOLCHAIN_DEFAULT = "mingw"`).
  `VCS_TOOLCHAIN=msvc` keeps Visual Studio's build (and setup_vs.py, the launcher's Visual Studio step behind it) for
  comparison; it goes when nothing needs it any more. A player's existing build folder is set up again from scratch on
  the first build after the switch (one full build).
- Switching compilers sets a build tree up from scratch (`toolchain.drop_other_compiler`: CMake can't change a tree's
  compiler), keeping its `--opt`.

## 2026-11-04 - D3D11 drops the GS shader's depth output too (issue 4: fire invisible on AMD)

GitHub issue 4: fire, explosions and flame-thrower flames invisible on an RX 6600, and on a laptop's Radeon iGPU while
the same laptop's NVIDIA GPU drew them; the reporter's second video shows them drawn but hidden by everything except the
sky. Not reproducible here: the GTX 1080 Ti and WARP (`PS2X_RHI_WARP=1`) both draw them, and the D3D11 debug layer
(`PS2X_RHI_DEBUG=1`, its messages now in the log) is silent. What the F12 draw log shows: the fire is GS sprites from
the 0x02c0 re-draws (CPU-made vertices, Z ~0.0016-0.0036, ZTST GEQUAL, no Z write, `Cs*FIX + Cd`), depth tested
against the world and models, which the native vertex shaders write; the sky's pixels keep the frame's depth fill (0).
"Visible only over the sky" is the fire's depth losing to any written depth. Every draw wrote its depth from the pixel
shader (SV_Depth) - the GL renderer's precision fix (2026-11-02 above): the driver's depth-export path, late Z, and on
AMD a hierarchical Z kept beside it; the one step where the drivers that work and the ones that don't can differ, and
it was only ever needed for GL. So D3D11 uses the rasterised depth like Vulkan: the same value (the vertex depth,
screen-linear; `pos.w` 1 for recorded draws, `pos.z = depth * w` for native ones), and better defined than the shader's
`noperspective` attribute where the hardware clips a native triangle (the new vertices' attribute values are the
driver's choice). It also turns early depth rejection on. Reference scenes with and without the shader depth on the
GTX 1080 Ti: statics 0.00-0.02, moving ones only their timing; the fire scene the same. A best guess without an AMD GPU to check on:
`PS2X_HW_SHADER_DEPTH=1` brings the shader depth back, and the reporters are asked to try the build.

## 2026-11-05 - 0.2.1's build problems (issues 62, 66-70, 73): the build's Python, long folders, half updates

Six reports in the day after 0.2.1, four of them one error: `Could NOT find Python3 (missing: Python3_EXECUTABLE
Interpreter)` at `src/render/CMakeLists.txt:8`. 0.2.1 compiles the D3D11 shaders with `tools/hlsl_dxbc.py` (no Windows
SDK), so CMake needs a Python for the first time, and it looks for one of the PC's own: a release runs on its bundled
embeddable CPython (`python/`), which isn't on PATH. A PC with no Python failed (issues 68, 70, 73); one with a broken
Python got "Cannot use the interpreter" (67). Reinstalling Python "fixed" it for the reporters. Reproduced in a 0.2.1
release folder with every other Python hidden (an empty `USERPROFILE`, since `build_env` adds scoop's shims): the same
message. **The build now hands CMake the Python it runs on** (`-DPython3_EXECUTABLE`, `toolchain.python_defines`, for
the game's and the launcher's trees), and a tree whose cache has another Python or none (a 0.2.0 tree, a failed
configure) is configured again once (`toolchain.python_changed`). Checked: a fresh tree, a retry after the failed
configure, a tree set up by 0.2.0 - each configures and builds `vcs_render` with the bundled Python; the next build
doesn't configure again.

Issue 69 (a Quiver install, a 105-character folder) had no 0.2.1 logs at all: the build stopped before configuring. 0.2.1
also downloads llvm-mingw, unpacked into `build/build-tools/<folder>.unpack/<folder>/`; its deepest files are 117
characters, so below a folder longer than 86 characters 77 of them pass MAX_PATH, and `zipfile` stops with a
FileNotFoundError unless Windows' LongPathsEnabled is on (it is on this PC, so not reproduced here). The unpacking now
uses `\?\` paths (`toolchain.long_path`); the program's path handed to PATH and CMake stays the plain one. The likely
cause, not a confirmed one.

Issue 66: the update from 0.1.1 stopped halfway - `VERSION` said 0.2.1 (the zip's 12th file), `tools/doctor.py` was
0.1.1's (its 4,938th). Updating a copy of 0.1.1 here works, and neither zip marks a file read-only, so something on that
PC held a file (a scanner, a synced folder); the report has no update log to say which. `update.py apply` now retries a
file held open, skips and names one it still can't write, writes `VERSION` and `RELEASE.json` last and only when every
other file is in, and fails with `update_incomplete` - the folder still says it is the old release, so the launcher
offers the update again. Checked with `tools/doctor.py` held open with no sharing: the update names it and stops, the
folder stays 0.1.1; the retry completes it. A player whose folder is already mixed updates with the old `update.py`:
extracting 0.2.1 into a new folder is their way out.

Issue 62 (0.2.0, still in 0.2.1): `UnicodeEncodeError: 'charmap' codec can't encode character '꞉'` - a disc
named with "꞉". The launcher sets `PYTHONIOENCODING` / `PYTHONUTF8`, but runs the tools with `-I`, and isolated mode
ignores every PYTHON* variable: stdout to the launcher's pipe was cp1252. `toolchain` now sets stdout / stderr to UTF-8
(errors replaced) itself; `report.py` reads `doctor.py`'s output as UTF-8.

## 2026-11-05 - The game's pause menu: Escape is Start, Enter selects, the mouse points and clicks (the user's request)
The user asked for the in-game menu on Escape, Enter as select in it, the cursor shown and the menu usable with the
mouse, the gamepad unchanged. "The in-game menu" is the game's own pause menu (the F2 settings menu already had the
mouse, Escape and Enter): Start was Enter and the menu took only the pad's buttons, with the cursor captured.
- **Start's default key is Escape** (Keyboard page, rebindable as before). Players who never changed Start get it with
  the update (vcs.ini keeps changed keys only). Enter outside the menu is nothing now.
- **In the menu, Enter = Cross and Escape = Triangle** (while held, pressed in the menu): Escape goes back a level and
  closes the menu from the tab bar - the game's own Triangle does exactly that (FUN_003b5138), and it is a PC game's
  Escape. Start (any page) would close it at once from deep in a save dialog; the gamepad's Start still does.
- **The mouse plays the pad** rather than changing the menu's state: pointing walks the D-pad to the item (a search over
  the items' own focus moves), a click is Cross / D-pad right / slider steps, the right button Triangle, the wheel L1 /
  R1 on the map. So the game's sounds, page previews, wrap-arounds and save flow are its own, and nothing it keeps can
  be left inconsistent. The cost: a step every other game frame (~70 ms at 30 fps for a far item). The map's pan is the
  one direct write (+0xE4 / +0xE8, clamped to the range the D-pad reaches - past it the game draws the map wrong).
- **Where items are** comes from their drawing (each item class's draw hooked; its Im2D primitives make its box), not
  from their layout rects: those are wider than the text and overlap (the tabs' are 100 units wide, 53 apart). The
  boxes are in the menu's 640x448 drawing, which fills the 4:3 area the frame shows the menu in; the cursor is mapped
  there through the presenter's `fitRect`. (A 480x360 "shrink" applied on top of that at first pulled the hit areas
  towards the middle of the screen - the player had to point a little off an item; game-internals.md 6m. The menu
  check now moves the cursor in window pixels, through the same mapping as the real cursor.)
- Would change: a menu page whose items can't be reached by focus moves (none seen) would need its own handling.

## 2026-11-06 - Car reflections: Smooth above 1x by default, the PS2's sampling at 1x (the user's request)

Players reported flickering car reflections (GitHub). The user thought this was the PS2's own behaviour made worse by
the resolution, and asked for a fix that keeps the PS2's behaviour at its resolution. Found (game-internals.md 6g): the
flicker is the PS2's. A flat panel shows one texel of a 128x128 map that the game samples from the frame with four
bilinear taps per ~5 x 3.5 pixels, and the renderer reproduces that exactly at every resolution. Above 1x the rest of
the picture is clean, so the panels' jumps stand out.

- **The fix filters the map's source, not the effect's strength.** Toning the reflection down at high resolutions would
  make the flashes fainter but no rarer, and the paint duller than the PS2's even when nothing moves. A tent over each
  map texel's footprint keeps the reflection's average colour (the same strength, the same picture in a still frame)
  and takes away the jumps: a texel's swing under sub-pixel motion falls ~2.7x.
- **Always Auto, no setting** (the user's choice, the same day: a first build had "Car reflections: Auto / Original /
  Smooth" on the Graphics page; the user asked to remove it and keep Auto). Original at 1x internal resolution - the
  authentic picture - and Smooth above it, where the player has already chosen a cleaner picture than the PS2's. "1x"
  goes by the frame's height: widescreen's 1x is wider, not finer. The internal resolution is the switch: 1x gives the
  PS2's reflections. `VCS_REFLECTIONS=0|1|2` (Original / Smooth / Auto) picks for one run, for comparisons; an old
  `graphics.reflections` line in vcs.ini is left alone and ignored.
- **Not done:** a blend over time (the PS2 has none: the map is overwritten every frame). It would also lag reflections
  behind the camera. Worth trying only if Smooth is not enough.
- Would change: if players find Smooth too soft compared with the original's crispness, a narrower footprint (a box of
  one texel instead of the tent over two) is the middle ground.

## 2026-11-06 - The game's own widescreen option off for good; the cutscene bars a setting (the user's decision)

The user noticed that the pause menu's Display > Widescreen (the PS2's own option) removes the cutscene black bars,
and that it "runs counter to" F2's widescreen. They asked for everything it does to be found (game-internals.md 5a),
and suggested keeping only F2's widescreen, with what is worth keeping as separate options.

- **What it is**: the PS2's mode for a 16:9 TV that stretches the 640-wide picture. Vert- 16:9 3D (the same horizontal
  view, the vertical cut), the HUD, text, pause map and radar blips 3/4 wide, the `_WIDE` menu layouts, wider scope
  masks - and no cutscene letterbox. On a PC the picture is not stretched by the screen. F2's Hor+ view and the
  renderer's HUD squeeze do the same job at any ratio, so with both on, everything is squeezed twice.
- **Kept off, not offered**: only the bars are a real choice (a 4:3 TV had them, a widescreen TV didn't). The
  preference is held at 0 at its setter (so a save that has it on can't turn it back on, the way the volume levels
  are kept, 9c) and at the frame kick. The menu row is hidden rather than left locked: a row that shows Off and
  ignores the D-pad looks broken (the user's choice between the two).
- **Cutscene black bars: On / Off, default On** (the user's choice; offered with an Auto that followed the aspect
  ratio): the PS2's 4:3 look in every ratio. At 16:9, On frames cutscenes at about 2.37:1; Off shows the whole
  picture, as the PS2's widescreen mode did. It is in the Authentic preset's reset (On); Enhanced and Light leave it.
- **Locked On at 4:3** (the user's request, same day): at 4:3 the bars are the original picture and Off has nothing
  to show beyond it, so the setting is greyed out showing On, with "Always on at 4:3..." in its help. In widescreen
  (16:10 and up; Match window when the window is wider than 4:3) it is the player's choice again. The saved value is
  not changed by the lock: a player who chose Off at 16:9, went to 4:3 and back gets Off again. The registry got a
  general `Spec::locked` / `lockedValue` / `lockedNote` for it (settings.h) - a setting another one decides.
- The reference stays reachable (rule 1): `VCS_WIDESCREEN=game` is the game's own handling, `VCS_WIDESCREEN=1` forces
  the PS2's mode, `VCS_HIDE_WIDESCREEN=0` keeps the row.
- Would change: if a 4:3-display player wanted the anamorphic HUD for a TV that stretches (unlikely on a PC), the
  preference could come back as an Advanced setting. The code is a switch away.

## 2026-11-07 - Button prompts by device; an Enhancements page (the user's request)

The user asked for a new F2 page, **Enhancements**, for settings that change how the game works rather than how it
looks or is controlled, starting with button prompts for keyboard / mouse and Xbox pads that follow the device in use,
built so that more glyph sets can be added. They asked for the investigation and a proposal first (game-internals.md
6n), then approved it as proposed.

- **Default Original** (authentic by default): the PS2's text, the game's own code; the hooks stay in but pass
  through. Automatic is one choice away; PlayStation / Xbox / Keyboard & mouse fix a set.
- **Glyphs follow the bindings, not fixed pictures**: the keyboard set shows the keys bound (the mouse button where one
  is bound - attack, target -, the Driving keys for a vehicle's actions, Enter / Escape in the pause menu, the mouse
  for the right stick while the mouse camera is on); the pad sets show the physical button the Gamepad page maps the
  PS2 button to. A Nintendo or unknown pad shows Xbox glyphs until a set of its own is made (one glyph table in
  vcs_glyph_art.cpp, one case in `specFor`).
- **Drawn by code, no image files** (as the launcher's icons): no Sony / Microsoft artwork is shipped; the shapes are
  generic (a dark disc with a coloured symbol or letter, pills for shoulders and triggers, keycaps, a mouse). The atlas
  is made at the window's size, so glyphs stay sharp.
- **The game lays them out**: the font's own glyph table is extended (tokens per control), so centring and line breaks
  are the game's; only what the renderer samples changes. The tokens name a control, not a device, so a help box that
  is already up changes with the device.
- **The PSP's "L button" / "R button"** in the PS2's text are taken as L1 / R1 (where another configuration of the same
  action says R1). Not checked against the pad code; a wrong one shows L1 where the game meant another shoulder.
- Not yet: the Controls page's controller picture (a keyboard list there would be a page of its own); the loading
  screen's "Press ~X~" shows the on-foot Cross key (Enter only works inside the menu).

## 2026-11-07 - Traffic / pedestrian density and draw distance (the user's request)

The user asked for two enhancements, more cars and peds and objects drawn further away, keeping performance and the
original game at the defaults, with engine changes allowed. Investigated first (game-internals.md 6o, 12d); the user
chose two density sliders and, for draw distance, the LOD scale with the far plane tested before it is offered.

- **Three sliders on Enhancements > World**, 100% (the original) to 300%: Traffic density, Pedestrian density, Draw
  distance. 100% writes the game's own values; `VCS_WORLD_DETAIL_HOOKS=0` writes nothing (rule 1).
- **The scripts' multipliers are left alone**: missions set them to 0 to clear the streets and saves carry them, so
  the setting scales the spawners' own constants (0.7 / 0.4) instead - a mission's 0 stays 0.
- **The caps grow with headroom**: cars at most 28 (the game stops at 30 random cars itself), peds 45, and only while
  16 slots stay free in each pool (set right before each spawner runs), so a mission's CREATE_CHAR / CREATE_CAR never
  meets a full pool and the game never deletes the closest car or ped to make room (it does below 8 free, visible or
  not - the first version stopped spawning only there, too late).
- **Cars in view are never removed** (the user's report: cars vanished ahead while driving, 2026-11-07): the game
  removes on-screen cars 160 m out, which a longer draw distance shows. With any World setting above 100% that
  removal waits until the car is out of view or beyond the distance cars are drawn at. `VCS_WORLD_KEEP=0` is the
  behaviour before, to compare; the module counts and logs any car that vanishes in view.
- **Traffic stays modest**: the density limit rarely binds for cars - the spawn spots 110 m ahead do. The spawn chance
  per road class is scaled too (class 0, next to no traffic, kept), which helps while driving; more would mean
  changing where cars spawn, a deeper change than an option should make. Release notes should call it a modest change.
- **Draw distance is the LOD scale only**: the game's own unused `0x4887F8` (VC's ms_lodDistScale), so the same
  mechanism as the PC versions' slider. No automatic step back (the user's call, 2026-11-07: this is a PC, the setting
  is what it says): the first version lowered it near the packet buffer's limit, where the game drops sectors, but
  300% used at most 220 KB of the 921 KB, so the setting is applied as it is.
- **The far plane stays 2000**: moving it (and so the fog) showed the far water and land as dark patches the fog hid
  on the PS2, and thinned Vice City's haze. Kept as a test switch, `VCS_FAR_CLIP=<metres>`; it would need its own
  work on the distant LOD's colours to be an option.

## 2026-11-08 - The NPC drive-by's fire rate (the user's request)

The user saw NPCs in cars fire SMGs far too fast, faster at high frame rates, and asked whether it was frame timing or
the original game. Both (game-internals.md 12c): the NPC drive-by fires a shot every frame - 30 a second on the PS2,
2.7x the SMG's own 90 ms, and 144 a second at 144 fps. The user chose a PS2-rate fix and the gun's rate as an option,
on the Enhancements page.

- **One setting, Enhancements > Combat: Drive-by fire rate.** *As on the PS2* (default) is the original game as it
  played on the PS2, at any frame rate: a shot per 1/30 s. *The gun's own rate* fixes the original bug: the weapon's
  firing rate, as the player's own drive-bys use. No choice keeps "a shot every frame" - that is a frame-rate bug of
  this port, like the others fixed without an option; `VCS_DRIVEBY_HOOKS=0` brings it back for comparison (rule 1).
- **Hooked at CWeapon::Fire, only for the two NPC drive-by calls** (by return address), not at the drive-by update:
  skipping the whole update would also hold back its burst gate and animation, so bursts would end late.
- **The next shot is due one interval after the last was due**, not after it came, with 2 ms tolerance: the rate is
  exact whatever the frame length, and at 30 fps every frame (33 / 34 ms) still fires - identical to the original.

## 2026-11-10 - A round of GitHub issues: 41, 89, 90, 91, 93, 102, 112 (the user's request; the choices below are Claude's)

The user asked for the most common GitHub issues to be gathered and the first two steps of the plan done: a triage
sweep and the small fixes with clear causes. No choice below was put to the user first; they are for review.

- **Purple Haze (41): the softlock is fixed at every frame rate, the cutscene only above 30 fps.** The main script's
  wait for an audio line that never ends once the line's slot is freed is a hang on a PS2 too, whenever a speaker dies
  between "load" and "loaded"; ending it changes nothing else, so it has no option and applies at 30 fps as well
  (`VCS_SCRIPT_FIXES=0` is the reference, rule 1). The truck is parked early only while the frame is not the PS2's:
  at 30 fps the original plays as it always did. Parking it - with the mission's own commands, behind the fade, half a
  second before the mission does - was chosen over making the thugs unhurtable (they would still be knocked down and
  the scene would fade in on a man getting up) and over chasing the collision itself (the hit differs with the step
  inside the engine's collision response; noted as open in game-internals.md 13).
- **Rotor blades (102) count once per 1/30 s rather than being scaled.** The blade test hands a damage impulse to
  VehicleDamage, which has a threshold: an impulse scaled by the step would fall under it at high frame rates and do
  nothing at all. Letting it through on one frame per PS2 frame keeps each hit what it was and the rate what it was on
  the PS2; the sparks and the push off the wall stay every frame. Under `VCS_PHYSICS_TIMESTEP`, with the other
  per-frame vehicle physics.
- **The haze (93) is a screen effect in every HUD mode**: stretched across the picture with the HUD at Keep 4:3 too,
  as fades and the scope's mask are. `VCS_HAZE_FILL=0` is the old behaviour.
- **The radio on the mouse wheel (90) is part of Keyboard > Mouse wheel**, not a setting of its own: the wheel did
  nothing in a vehicle, the option is on by default and a wheel bound to a control still wins. Down is next, as for
  weapons. The step back goes through the game's own retune (its click, the station's name, its 30-frame wait) rather
  than a stream switched behind its back: one byte written, and written back when the game did not take the press.
  Nine quick presses of L1 would also land one station back, but click nine times.
- **F12 (89) needs `PS2X_HW_DUMP_KEY=1`** rather than a setting or another key: the frame dump is a developer's tool
  (hundreds of lines on stderr, the render targets as files next to the game), and every free F key is somebody's
  screenshot key. `PS2X_HW_DUMP=<frame>` works as before.
- **The graphics card (112): Fastest by default, a setting to change it.** A game should start on the dedicated card;
  Graphics > Graphics card offers Power saving (the integrated one) and System default (what it was: Windows' default
  adapter, which follows Windows' own Graphics settings and the driver's panel). Chosen through DXGI
  (`EnumAdapterByGpuPreference`, Windows 10 1803+) with the default adapter as the fall-back, not through the
  `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` exports: those cannot be turned off by a setting. The
  launcher stays on the default adapter (it idles; no reason to wake the big card). Not checked on a PC with two cards -
  the dev machine has one; the log's `[rhi] graphics cards` line is there for the reporter's answer.
- **vcs.exe's icon and details (91)** are the launcher's icon and the release's VERSION. The version reaches the
  resource file through a generated header, not compile definitions: a definition on the target would be on every
  generated file's command line and rebuild the whole game at each release.

## 2026-11-10 - The issues round checked on Linux (the user's request; the choices below are Claude's)

The user asked for the latest changes to be reviewed and made sure of on Linux. The laptop's desktop was locked while
this ran. No choice below was put to the user first; they are for review.

- **The lock is not worked around.** Locked, the user's session has neither the graphics card (logind hands
  `/dev/dri/*` to the greeter) nor a window manager that maps windows. No VT switch, no unlock, no change to the
  device's permissions: the checks that read the game's memory or its log ran on the null renderer (the game at full
  speed, no picture), one picture came from llvmpipe, and what needs the card is listed as not checked (PROGRESS.md
  milestone 9, LINUX.md "A locked desktop").
- **Hidden runs, against the laptop's rule of shown test windows** (2026-10-30): a locked desktop shows none - a shown
  window never leaves `SDL_ShowWindow`. The rule stands for an unlocked desktop.
- **The Vulkan backend prints `[rhi] graphics cards` too**, as D3D11 does since issue 112: the setting's comment,
  PROGRESS.md and the entry above name that line as the answer to "which card did it take?", and on Linux there was
  none. Each device comes with its kind (Vulkan's device type: dedicated, integrated, virtual, software) and, when it
  cannot be used, what it lacks. A second line says when only Mesa's software renderer is left: found here by
  accident (the locked desktop), it is also what a player without a Vulkan driver gets - a few frames a second, and
  until now nothing in the log to say why. (2026-11-12: made one line with the one the cards-by-name change brought;
  that entry's last point.)
- **No window icon for the game on Linux - left to the user.** Issue 91 was Explorer's blank vcs.exe; its fix (the
  exe's resources) also gives the Windows window its icon, and Linux has no counterpart: the game's window shows the
  desktop's generic one. `SDL_SetWindowIcon` with the pixels `launcher/make_icon.py --header` already makes for the
  launcher would do it on X11 (and on Wayland where the compositor has xdg-toplevel-icon). Not asked for, so not added.
- **0.2.5's draft notes**: the graphics card bullet says what Linux gets (it already took the dedicated card; Fastest
  and Power saving), and the vcs.exe bullet is prefixed `Windows:`, as AGENTS.md asks of one-platform bullets.

## 2026-11-10 - The RSL recordings get a memory limit (GitHub issue 115; the user's go-ahead, the choices are Claude's)

The user asked for issue 115 (vcs.exe at 5.3 GB) to be looked into and, shown the cause and a plan, said: implement
the recording cap "as long as it doesn't reduce your ability or prevent you from working on the game". The numbers
are in PERFORMANCE.md 31; the choices:

- **A limit in bytes, least recently used first** - not a count (recordings run from a few hundred bytes to hundreds
  of KB), not an age (standing still for an hour must cost nothing), and not recordings keyed by what the block holds
  instead of where it is. That last one would remove the cause - the same model at a new address would find its
  recording - but every block would have to be hashed to be looked up, which is the work step 4's skip was made to
  avoid, and it changes what the verify tools compare. The limit changes nothing about a recording while it lives.
- **256 MB by default.** One place needs 12-36 MB of recordings, so 256 MB is several districts: driving back and
  forth across town replays what it replayed before (on the check's route about as many blocks are recorded as with
  no limit at all). The game then levels off at 650-700 MB on the dev machine at 3x, against 300-350 MB with no
  recordings and 842 MB and rising after 15 minutes without the limit. With 64 MB it levels off at ~470 MB and
  records about twice as many world blocks. At a 60 fps cap that costs nothing that shows (the stutter rig at 24
  MB against no limit: the same); above 100 fps, where the render worker is the limit and a replay is what saves its
  time, it was not measured - so the default is the size at which hardly more is recorded than before, and about
  200 MB is what a smaller one could still give back.
- **Not a setting in the menu**: nothing a player should have to choose. `VCS_RSL_RECORDINGS_MB` for a run (0: no
  limit - the behaviour before, rule 1's switch back).
- **A recording is dropped in two steps** (its confirmation withdrawn first, freed when the game thread has applied
  that) rather than leaving it to the worker's existing fall-back, which reads the block from guest RAM: that
  fall-back was written for a case that never happens (0 of 12 million skipped blocks) and reads memory the game
  thread may be writing by then. With the two steps it still never happens, and `VCS_RSL_SKIP_VERIFY` keeps meaning
  what it meant: a skip decided for a block whose recording is gone would be reported as wrong.
- **The condition - nothing taken from the tools.** Recordings, replays and both verify modes work as before; a
  recording that is gone is one that was never made. `VCS_RSL_WORLD_LOG=1` gained a `memory:` line (held, dropped,
  recorded again, the slowest sweep) and `tools/memcheck.py` is the check that it stays fixed. Tests that pin
  `VCS_TIMING=0 VCS_FPS=30` are untouched; none of the rig's runs reaches 256 MB of recordings.
- **Not done**: an `Op` is 104 bytes for every UNPACK because each carries a mesh and a raster name it does not use;
  a leaner layout would hold more per MB (not measured). Left, as the limit makes it a question of tuning.

## 2026-11-11 - A mouse camera of its own on foot: built in three rounds, then taken out (the user's decision)

The user asked for the on-foot mouse controls to be upgraded as the vehicle camera had been: the camera free of the
player, back behind him while he moves, the gamepad as before, the camera no longer stopping at a building, and all
of it feeling like PC controls rather than a pretend gamepad. Three builds the same day, each played by the user:

1. The movement keys kept the direction they were pressed at whatever the camera did (the user's own choice among
   three); the mouse only looked. Verdict: "it would be better for the player character to always be moving in the
   general direction of the camera. Being disconnected feels bad."
2. The keys followed the camera again; the camera held still after the mouse or a change of keys, then turned behind
   him slowly. Verdict: a good direction, but the camera should follow where he travels more actively, "a bit closer
   to the gamepad camera" - travelling east or south it stayed where it was.
3. The camera came round behind where he travels at once, the keys keeping their heading through that turn only.
   Then: "revert all changes that have been made today related to the camera while walking."

So all of it is out again - the module, its setting (Walking camera return), its checks, the notes - by one revert of
the three commits (6948b7a, 993d670, ea5628e stay in the history). On foot the mouse turns the game's own camera at
the pad read and steers the player, as since 2026-10-29. **The wall stop is back with it** (the mouse's camera
standing at a building's edge): its fix was part of the module. Kept, as the rules of this repository ask: what was
found out about the game (game-internals.md 12b "On foot: the stick, the camera on its string, walls"), the open item
in PROGRESS.md, and this entry. Not kept: the `--only` option of `tools/controls_check.py` and the gamepad-driven
`framerate_audit.py` footcam scenario, which came with those commits.

For another attempt: every build was exact to its specification in the checks and still wrong in the hand - the
question was never the mechanism. Worth settling by playing, before anything is built to completion: whether the
camera should turn by itself at all on the keyboard and mouse, and what a key should mean once it has.

## 2026-11-11 - The wall fix alone, on the mouse look as it was (the user's go-ahead)

After the revert above the user was asked whether the wall fix should come back by itself and said: "Yes, wall fix
only." So it is the smallest thing that does it, in the pad read where the mouse already turns the camera - no hook,
no camera of its own: while the mouse turns the on-foot camera the update is told to take the path it takes for a
turned stick (the pull-in toward the player), through the game's own switch for it (`0x487E58`). Claude's choices:

- **A tail of 300 ms after the mouse's last turn.** A slow hand, or a high frame rate, leaves frames without a count
  between frames with one; on those the game's own turn away from a wall he walks along would have worked against
  the mouse. Standing, where the user saw the fault, it makes no difference.
- **Nothing else of the on-foot camera changes**: the game's swing behind him, its string, the keys following the
  camera are as they were. With the mouse at rest the game's own handling of walls is back.
- `VCS_FOOT_WALL=0` is the switch back (rule 1); not a setting - there is nothing to choose.

## 2026-11-12 - Graphics > Graphics card lists the cards by name (the user's request)

- **The cards, not "Fastest / Power saving / System default"**: the user asked for the list of cards the game can see,
  with "(integrated)" after the CPU's own chip. The first is the fastest (DXGI's high-performance ranking; Vulkan:
  discrete before integrated) and the default, so nothing changes for a player who never opens the setting. "System
  default" (Windows' default adapter) went with the old choices; Windows' own Graphics settings no longer steer vcs.exe.
- **vcs.ini keeps a card id, not a place in the list**: settings are ints, and a list's order changes when a card is
  added, removed or ranked differently. The id is a hash of the PCI vendor and device ids and which of several identical
  cards it is (`rhi::gpuId`): the same every start and under D3D11 and Vulkan (the list comes from D3D11 where it is
  built - its names are Windows' -, the Vulkan backend matches the id against its own cards). A card no longer there is
  an id no choice has: the setting falls back to the fastest. The old values 0 / 1 / 2 are ignored the same way (never in a
  release).
- **"(integrated)" from DXCore** (`dxcore.dll`, Windows 10 2004+, `IsIntegrated` looked up by the DXGI adapter's LUID):
  DXGI has no such flag, and a guess from the dedicated memory size fails on AMD APUs with a large carve-out. DXCore's
  header isn't in llvm-mingw, so the two interfaces' first methods are declared in rhi_d3d11.cpp; checked on the dev
  machine with a scratch program: IsHardware 1 for the GTX 1080 Ti, 0 for Microsoft Basic Render Driver, IsIntegrated 0.
  An integrated chip itself is not checked (none here). Vulkan: the device type. Older Windows: no mark.
- Software renderers stay out of the list (Microsoft Basic Render Driver, llvmpipe), as do Vulkan cards older than 1.3.
- **Merged with the Linux check of 2026-11-10** (2026-11-12, on the laptop; these choices are Claude's and were not put
  to the user - for review): that check had given the Vulkan backend a `[rhi] graphics cards` line of its own (every
  device with its kind, "(choice: fastest / power saving)"), and this change gave it another. One line is kept, in
  D3D11's form - the menu's list with the ids, "; using <card>" - plus what only the Linux one said: what a listed card
  that cannot be used lacks, and after "; not in the list:" the devices the menu leaves out (llvmpipe, Vulkan older
  than 1.3), so a log still shows that only the software renderer was there. "(VCS_VK_DEVICE)" follows the card's name
  when that switch chose it. The software renderer's warning line stays as it was.
  Seen on the laptop's HD 520 (PROGRESS.md milestone 9): the "(integrated)" mark in the log and in the menu - where the
  closed row cuts it to "(integr", Mesa's name being longer than the box; left as it is, for the user to decide.
  0.2.5's draft notes: the graphics card bullet describes the list by name, for both systems (it still said Fastest /
  Power saving / System default, which no release ever had).

## 2026-11-12 - Private Git checkout updates from the launcher

- The user wants startup checks and updates of this project from the private GitHub repository. A Git checkout follows
  `ChrisM-Pek/gta-vcs-recomp` main through the existing `private` or `origin` remote, using Git's existing credentials;
  the launcher defaults to a startup check. An available update is applied only on request and only as a fast-forward,
  without overwriting local work. The chosen disc determines the rebuilt game; the launcher is built in a separate
  staging directory before replacing the running executable, with a restart offered. Release folders retain the
  existing release updater. The check and application are described in LAUNCHER.md.

## 2026-11-12 - Public European fork

- At the user's request, `ChrisM-Pek/gta-vcs-recomp-europe` is the public fork and the source of its launcher updates
  and future releases. The original `gothamor/gta-vcs-recomp` is kept as `upstream`, and the earlier private repository
  is retained separately. The clone's `origin` points to the European fork, so release metadata identifies the fork.

## 2026-11-12 - HD textures for the 2D layer: PCSX2's packs, off by default

- The user's report: the HUD stays the PS2's low-resolution art even at a modern internal resolution. The native HUD
  layer draws the HUD at the output size but samples the disc's textures (font atlas 256x256, digits 128x64, icons
  and radar tiles 128x128 or less), so the last 640x448 thing on screen is the HUD's art.
- **Packs are PCSX2's.** They are named by PCSX2's own texture hash (`GSTextureCache::HashCacheKey::Create` +
  `GSClut::Read32`), so the port reproduces that hash from its GS memory and finds existing packs as they are - the
  user's "HD HUD - SLUS-21590 - PS Icons" pack works unchanged, on the PAL disc as well (the HUD art is the same
  data; only the old TCC bit of the name differs, which PCSX2 itself ignores). An own naming scheme would have
  worked too, but every pack in the wild would have had to be converted, and the hash is what makes a texture
  recognisable across discs and languages.
- **Off by default** (authentic unless asked for: the project's rule), Mods > HD textures (HUD).
- **Alpha stays in the PS2's 0..128 scale** (these packs are dumped from it) instead of PCSX2's 0..255: the port's
  shaders use the PS2's scale, and rescaling would make every HUD glyph look too opaque or too faint.
- **Caps**: 8x the PS2 texture and 2048 a side, larger images area-averaged down on load - a 4096x4096 font atlas is
  64 MB of pixels for a HUD drawn at 24-40 px a glyph.
- **The geometry does not change**: the layer still snaps its vertices to the PS2's pixel coverage (that is what
  lines the radar's mask, scissor and map up). So the art gets sharper, the layout stays the PS2's.
- The pack's images are **read in the background** (2026-11-12, a second pass): the first ask of a texture queues its
  file on a reading thread and the PS2's art is drawn until the image is ready (a frame or two later), then the
  replacement appears. Decoding a 4096x4096 PNG on the GS stage was a 50-200 ms hitch each time a new HUD element
  showed; measured at 60 fps, the pause menu's first opening went from a 35.8 ms frame to 19.9 (idle 17.3). The
  texture's own upload goes to the helper threads (`makeTextureEarly`), so a 16 MB image does not hold a frame
  either. `VCS_HUDTEX_SYNC=1` reads on the GS stage as before, to compare.
## 2026-11-12 - Mods: installed and switched in the launcher, one switch in the game

- Players want the HD HUD in one step, but the pack is third-party art: the launcher's **Mods** sheet (title bar, next
  to Saves and Settings) links the pack's GTAForums page and installs a chosen `.zip` or `.7z` with `tools/mods.py`
  into the build folder (the game's `textures/`), turning the game's **Mods > HD textures (HUD)** setting on so the
  next start shows it. Nothing of the game or the pack is redistributed.
- **One row a mod, with a switch**: a texture mod lives in `textures/<id>/` (the id is PCSX2's game folder,
  SLUS-21590) and its switch moves that folder to `textures-off/<id>/` - nothing is copied or deleted, both ways are
  instant, and the game, which only scans `textures/`, follows without a rebuild. `mods.json` in the build folder keeps
  the names, kinds, sources and dates the sheet shows. The game's own setting stays the single "use HD textures or
  not" (the authentic default), so the launcher decides *which* packs are there and the game decides *whether* to use
  them.
- **Prepared for other kinds**: `tools/mods.py` has a `kind` per mod and one place to detect it (`detect()`), one to
  install and one to switch it; today only `textures`, and a second kind is a case in those three places. The sheet
  lists whatever the tool reports, so a new kind needs no UI work.
- What would change it: packs shipped with their authors' permission, or the game offering to install one itself.

## 2026-11-12 - macOS: Apple Silicon, Metal, nothing to install but Apple's tools (the user's decisions; the rest Claude's)

- **The user's three**: Apple Silicon only (Intel Macs are left out: no machine to test them, and their GPUs would be a
  second set to check); a **self-contained build** - Apple's Command Line Tools are the only prerequisite, everything
  else is downloaded or built from pinned sources, never Homebrew; the release's launcher **signed ad-hoc** for now
  (players allow it once in System Settings > Privacy & Security), Developer ID and notarization later.
- **Metal, not Vulkan through MoltenVK**: the user asked for Metal for both programs. A Metal RHI backend
  (`src/rhi/metal/rhi_metal.mm`, Objective-C++ with ARC, no metal-cpp) keeps D3D11's behaviour as the Vulkan backend
  does, with the same tricks: an upload ring for write-discard buffers, passes that start at their first draw and are
  suspended around copies, a dummy texture for unbound slots.
- **Shaders: the one HLSL source through glslang and SPIRV-Cross, as MSL text compiled at run time.** The Command Line
  Tools have no Metal compiler (`xcrun metal` needs Xcode and a separate download); the OS's Metal framework compiles
  MSL. Fast math off (`MTLMathModeSafe`). glslang and SPIRV-Cross are built from the Vulkan SDK 1.4.350.1 tags, the same
  pins as the Vulkan headers. Bindings without the Vulkan shifts (Metal has 16 sampler slots and a table per kind).
- **Apple clang and ld64**, not Homebrew's LLVM: Homebrew's bitcode can't go through Apple's linker for ThinLTO, and
  lld isn't part of the Command Line Tools. `preserve_none` exists from Apple clang 17 (LLVM 19), checked by CMake.
- **sse2neon with its precise options** for the generated code's SSE (already wired upstream for ARM): VU0 / HLE verify
  agree with the Windows reference. FPCR through `ps2x::fpControl` rather than sse2neon's emulated `_mm_getcsr`.
- **macOS 13 or newer** (Metal 3 on every Apple Silicon Mac).
- **FFmpeg from source**: only the MPEG-2 decoder, its parser and swscale, static, LGPL - about a minute, no package.
- **16 MB thread stacks** for the game thread, the render worker and the GS stage (`ps2x::Thread`): macOS gives a
  `std::thread` 512 KB.
- **The launcher keeps macOS' window buttons** (traffic lights over the UI's title bar, a full-size content view)
  instead of drawing Windows' on the right; Command counts as Ctrl for its shortcuts.

## 2026-11-13 - macOS: vertical sync on by default (the user's decision)

- Display > **Vertical sync** defaults to On on macOS only (`kVsyncDefault`, `src/main.cpp`); Windows and Linux keep
  Off. The Enhanced and Light presets set it to that default too, so choosing one doesn't turn it off on a Mac, and
  Authentic resets to it. A vcs.ini that names the setting keeps its value. The Metal backend logs the mode it presents
  with (`[rhi] metal: vertical sync on|off`) at the first frame and at every change.

## 2026-11-13 - Full screen by default (the user's decision)

- Display > Screen > **Window mode** defaults to Full screen (borderless, on the monitor the window was last on) on
  every platform; it was Windowed. A vcs.ini that names the setting keeps its value, so a player who chose Windowed
  stays windowed. Shown test runs keep a window (`toolchain.test_window` sets `VCS_FULLSCREEN=0` unless given).

## 2026-11-14 - Effects as on the PS2 at any frame rate; "Reduce Post Processing Effects" turns it back (the user's decisions)

- The light trails (the motion blur read a PS2 frame back), the particles, lightning, clouds, camera shake, explosions
  and the other effects that counted frames run at the PS2's 30 a second at any frame rate (PROGRESS.md milestone 4):
  the user found them authentic.
- Above 30 fps or 1x the motion blur is a quarter weaker (`PS2X_HW_BLUR_STRENGTH`, 0.75): at speed in a car its ghost
  stood out a little more than on the PS2 (the user's request, "only slightly").
- Enhancements > Effects > **Reduce Post Processing Effects** (off by default, live; `enhance.reduce_postfx`,
  `VCS_REDUCE_POSTFX`): one toggle that puts all of it back as the port drew it before - the motion blur from the last
  frame at the game's strength, particles and the effects once a frame. The environment switches of each part
  (`PS2X_HW_BLUR_HISTORY`, `VCS_PARTICLE_TIMING`, `VCS_EFFECTS_TIMING`) stay for testing.
- The settings menu's label column widens per page (45% up to 60%) when a label needs it, so the setting's full name
  shows; pages whose labels fit keep 45%.

## 2026-11-14 - No Enhanced preset; Widescreen, light becomes Balanced at 2x (the user's decisions)

- Display > Presets loses **Enhanced** (Match window aspect and resolution, HUD at the edges, 4x MSAA, 16x AF, timing
  at the display's refresh): the user asked for it to be removed entirely. Authentic (PS2) and Widescreen, light stay;
  every setting Enhanced set is still on its own row. `presets.enhanced` was an action, never written to vcs.ini, so
  nothing in a player's settings refers to it. `tools/controls_check.py` checks the other preset in its place.
- "Widescreen, light" is now **Balanced** (`presets.balanced`, was `presets.light`; also an action, never stored) and
  sets the internal resolution to 2x instead of 1x; the rest as before (Match window, HUD at the edges, no MSAA, 4x AF,
  independent timing at 60). The user's request.

## 2026-11-14 - macOS releases are a disk image with an Applications link (the user's request; the rest Claude's)

The user asked for an image that lets players move the launcher into Applications. The macOS release is now
`VCS-Recomp-<v>-macos-arm64.dmg` instead of a zip: a Finder window with the app and a link to /Applications side by
side. The zip goes - no macOS release had been published, so no installed launcher looks for it (`update.py` still
takes a zip given by hand). Choices:

- **The layout without dmgbuild**: Finder reads the window (icon view, size, no bars) and the icon positions from the
  image's `.DS_Store`. The tools are standard library only (AGENTS.md), so `tools/dmg.py` writes that file itself (one
  B-tree leaf; checked by reading Finder's own files back with the same code), instead of `dmgbuild` / `create-dmg`.
  AppleScript to Finder (create-dmg's way) was left out: it needs a logged-in Finder, which CI's Macs don't
  reliably give.
- **No background picture** (an arrow): Finder finds it through an alias / bookmark to a file in the image, tied to
  the mounted volume - much more than the layout for little gain; the two icons side by side say it. Possible later.
- **HFS+, ULMO**: HFS+ as dmgbuild uses; ULMO (LZMA) needs macOS 10.15, the launcher needs 13 anyway.
- **The volume icon** is set with `SetFile` (the Command Line Tools'); without it the image keeps the plain disk icon
  and the release goes on.
- **`python -B` for the bundle's own scripts**: the first start's `update.py install` runs from inside the app and
  wrote `__pycache__` there, breaking the app's signature (found when the image's check refused the app). The launcher
  passes `-B` when the script is the release's own copy (macOS bundle or AppImage).

## 2026-11-14 - Upstream 0.3.0 merged into the European fork (the user's request)

- The user asked for the original repository's 0.3.0 release (35 commits after the 0.2.4 line, `upstream/main`) on the
  European fork. It is a merge, not a set of cherry-picks: the fork's own work (European disc, launcher, HD textures,
  mods, mission timers) and upstream's (effects and particles at the PS2's 30 a second, the radio, the missions, macOS)
  both stay, upstream history stays reachable, and the next upstream release merges the same way.
- The 11 conflicted files keep the fork's side where it is a fork feature and add upstream's: the launcher's
  `VCS_LAUNCHER_OUTPUT_DIR` also carries upstream's macOS bundle `Info.plist` step; `tools/update.py` keeps the
  git-based European update and takes upstream's macOS asset suffix for the release-folder path; `gs_hw` keeps both new
  switches (`gsHwSetBloomEnabled` and `gsHwSetReducedPostFx`); the docs keep both additions. `src/main.cpp` keeps the
  fork's PAL installation list and gains the three new timing modules.
- **PAL addresses for what upstream added are derived, not guessed.** `build/analysis/candidates.csv` (the 6,971 unique
  `--candidates` pairs of `tools/match_versions.py`) gives the function twins; an address inside one keeps its offset.
  Every hook target is then checked in the PAL executable: the word 8 bytes before a return address must be a `jal`
  whose target is the PAL twin of the US callee. Game data is the US address + 0x300, the offset `tools/framerate_audit.py`
  already uses, checked with the ELF where a matched function forms the address with `lui` + `addiu` / `ori` / load. The
  two functions without a candidate twin (CSpecialFX::Render2DFXs, CParticle::Update) had their inner return addresses
  placed by the callee's order inside them. A hidden PAL run at 60 fps logged the effects at 29.9 PS2 frames a second
  and particle sites throttled at their PAL return addresses, so the mapped sites are the ones the game calls.
- **The European build installs game modules from `main.cpp`, not `applyVcs`**: `PS2_REGISTER_GAME_OVERRIDE` names
  `SLUS_215.90`, so a SLES_546.22 boot never applies the US override list. Everything the European build needs is
  called in `main.cpp`'s `VCS_EXPERIMENTAL_PAL` branch - the new `installMusicTimingHooks`, `installParticleTimingHooks`
  and `installEffectsTimingHooks` included, in that order (the effects' AddParticle hook chains onto the particle one).
- macOS support from upstream comes along in the source (Metal backend, launcher, docs) but the fork still ships
  Windows and Linux only; nothing of it builds or runs here without an Apple host.
- `VERSION` is `0.3.0-europe`, following the fork's tag scheme; the release notes are
  `docs/releases/v0.3.0-europe.md`. Nothing was pushed: the user wanted to try the build first.
