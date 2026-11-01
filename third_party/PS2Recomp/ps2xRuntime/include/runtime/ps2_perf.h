// [gta-recomp] Performance instrumentation.
//
// Every game frame (or VBlank, if the game never marks frames) becomes one sample: wall time, the EE thread's time
// split into phases (game code / render submit / IOP / waiting), render counters, host-side frame and GL times and
// the audio buffer state. In game:
//   F3  toggle the overlay (numbers + frame-time graph)
//   F4  start / stop recording samples to perf_<date>_<time>.csv in the working directory
// PS2X_PERF_LOG=<file.csv> records from boot (plus a "[perf]" summary line on stderr every 5 s) - for scripted runs;
// tools/perf_report.py summarises a recording.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2perf
{
    // Where the EE (game) thread spends its time. Game = whatever no scope claims.
    enum class Phase : uint8_t
    {
        Game,
        Render, // DMA chains, VIF, VU1 microprograms, GS command recording
        Iop,    // IOP modules + SPU2 mixing
        Wait,   // pacing to the VBlank clock, all threads blocked
        Count
    };

    enum class Counter : uint8_t
    {
        Draws,       // GL draw batches recorded by the HW renderer
        Vertices,    // vertices recorded by the HW renderer
        TexUploads,  // textures decoded and uploaded to the GPU
        DmaTags,     // VIF1 DMA chain tags walked
        Vu1Programs, // VU1 microprogram runs
        // [gta-recomp] draw sources (gs_hw.cpp DrawSources), per executed frame: draws made from GS primitives (the
        // emulated draw path) and their vertices, those by where the packets came from (a VU1 program, a C++
        // reproduction of one, PATH2, PATH3), then the native renderer's: native draws, HUD-layer draws, GS draws
        // replaced by native post-effect passes
        GsDraws,
        GsVertices,
        GsVu1,
        GsHle,
        GsPath2,
        GsPath3,
        NativeDraws,
        HudDraws,
        PostDraws,
        PackedDraws,    // draws recorded from whole packets of the C++ VU1 reproductions (GS::submitPackedKick)
        PackedVertices, // ... and their vertices
        // [gta-recomp] textures made on the GPU side: the main thread's time creating (and releasing) them, the
        // kilobytes uploaded (issue #29: hitches after texture bursts)
        TexCreateUs,
        TexKBytes,
        TexDecodeUs, // [gta-recomp] the GS stage decoding them (mip chains and, D3D11, their creation: makeTextureEarly)
        Count
    };

    void add(Counter counter, uint64_t amount = 1u);

    // The render submission worker (runtime/ps2_render_worker.h) was busy for `ns`: worker_ms per game frame. Any thread.
    void addWorkerTime(uint64_t ns);
    // [gta-recomp] The GS stage (runtime/ps2_render_worker.h) was busy for `ns`: gs_ms per game frame. Any thread.
    void addGsStageTime(uint64_t ns);

    // Time spent until the scope ends goes to `phase` (nested scopes are subtracted from their parent).
    // EE thread only.
    class PhaseScope
    {
    public:
        explicit PhaseScope(Phase phase) noexcept;
        ~PhaseScope();
        PhaseScope(const PhaseScope &) = delete;
        PhaseScope &operator=(const PhaseScope &) = delete;

    private:
        PhaseScope *m_parent;
        uint64_t m_start;
        uint64_t m_childNs;
        Phase m_phase;
    };

    // EE thread: a game frame ended (game-specific hook). Without these calls each VBlank is a frame.
    void markGameFrame();
    // EE thread: a VBlank started.
    void markVBlank();

    // Main thread, once per host frame: how long the GPU renderer's command execution took.
    void hostFrame(double glMs);
    // Main thread, between BeginDrawing and EndDrawing: handles F3/F4 and draws the overlay when on.
    void drawOverlay(bool handleKeys = true);
    // [gta-recomp] Main thread, per host frame when the brightness probe runs (PS2X_FRAME_STATS / control channel):
    // the presented picture's mean luma (0..255). Counts black frames (summaryLine: black_frames / probed_frames) and
    // logs every run of black frames ("[frames] black run ...").
    // Returns true on the first frame of a black run or a dip (the moment worth a screenshot).
    bool noteFrameLuma(float mean, uint64_t hostFrame);
    // [gta-recomp] Main thread: the F3 overlay's state (settings menu).
    void setOverlay(bool on);
    bool overlayOn();
    // [gta-recomp] A second F3 panel under the perf numbers with the game's state (player, vehicle, camera, scripts):
    // the game-specific code fills `lines` (main thread, each host frame while the overlay is on). `color` is an
    // IM_COL32 value (0 = the panel's default); `header` lines start a section (drawn dim, a little apart).
    struct InfoLine
    {
        std::string text;
        uint32_t color = 0;
        bool header = false;
    };
    using InfoProvider = std::function<void(std::vector<InfoLine> &lines)>;
    void setInfoProvider(InfoProvider provider);
    // The overlay's numbers for the last second as one line (control channel "perf").
    std::string summaryLine();
    // Closes a running recording (end of run).
    void shutdown();
}
