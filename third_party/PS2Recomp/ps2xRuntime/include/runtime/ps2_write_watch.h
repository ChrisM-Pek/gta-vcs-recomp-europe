// [gta-recomp] Which 4 KB pages of guest RAM were written, without reading them (docs/FULLY_NATIVE.md step 4): guest RAM
// is allocated with the OS's write watch (Windows MEM_WRITE_WATCH: every write counts - guest code, host threads, the
// kernel reading a file into it - and costs nothing until asked). fold() takes the pages written since the last fold and
// bumps their generation; a page whose generation is unchanged holds the bytes it held then. Kept out of ps2_memory.h:
// the generated code includes that one.
//
// PS2X_WRITE_WATCH=0: guest RAM allocated as before; enabled() false, every generation 0 (nothing can be told clean).
// Linux (userfaultfd async write-protect + PAGEMAP_SCAN, ps2_write_watch.cpp) is opt-in: PS2X_WRITE_WATCH=1.
// PS2X_WRITE_WATCH_LOG=1: pages written per fold and the fold's cost, every 5 s.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2writewatch
{
    constexpr uint32_t kPageShift = 12u;
    constexpr uint32_t kPageSize = 1u << kPageShift;

    uint8_t *allocate(size_t bytes); // guest RAM (zeroed); new[] when write watch is off or unavailable
    void release(uint8_t *ram);

    bool enabled();
    // the pages written since the last fold get a new generation; returns how many. One thread at a time (the one that
    // kicks the frame's chain).
    uint32_t fold();

    // [gta-recomp] the generations, inline: the RSL replay's skip test asks per page of every marked block (~2,000
    // blocks a frame); an out-of-line call each was most of rslworld's `skippable` (PERFORMANCE.md 23). Set by
    // allocate / release; null (0 pages) while write watch is off.
    extern const uint32_t *g_generations;
    extern uint32_t g_generationPages;
    inline uint32_t generation(uint32_t page) { return page < g_generationPages ? g_generations[page] : 0u; } // page = offset >> kPageShift
    inline bool unchangedSince(uint32_t page, uint32_t gen)                  // false when write watch is off
    {
        return page < g_generationPages && g_generations[page] == gen;
    }
}
