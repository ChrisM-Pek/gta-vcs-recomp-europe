// Enhancements > World: traffic / pedestrian density and draw distance (docs/game-internals.md 6o, 12d).
//
// Density. Mission scripts set the game's density multipliers all the time (0 to clear the streets, 10 for the
// credits; CarDensityMultiplier 0x48A968, PedDensityMultiplier 0x487FF8), and saves keep them, so those stay the
// scripts'. The spawners multiply them by a constant of their own, read nowhere else: cars FUN_00430290 by 0.7 at
// 0x48A680, peds FUN_002B17A8 by 0.4 at 0x48A67C. Scaling those constants scales every script's density the same way
// (0 stays 0). Each spawner also has a cap: random cars + the other car counters below 16 (0x48A9A8), peds below 25
// outdoors (0x488004) / 30 elsewhere (0x488008); they grow with the setting up to a limit that leaves the pools
// (70 peds, 40 vehicles) room for the missions' own CREATE_CHAR / CREATE_CAR. The pool guard puts the original caps
// back while fewer slots than that are free, so random spawning stops first.
// Those limits rarely bind, though: a spawn happens only where a random 0..15 is at most the road's (or path's) class
// in the table 0x487498 = {0, 4, 7, 15} (FUN_00267E98, the spot search both spawners use), so most tries end there.
// Each spawner gets that chance times the setting (class 0 - next to no traffic - stays as it is): the table is
// written before the car spawner FUN_00430290 and the ped spawner FUN_002B17A8 run, the only code that reads it.
//
// Draw distance. CCamera::Process multiplies LODDistMultiplier (TheCamera+0x7DC: 70 / FOV, more when high up, at most
// 2) by 0x4887F8 - 1.0 in the executable, never written, the PS2 build's counterpart of VC's ms_lodDistScale - after
// it copied the value into GenerationDistMultiplier, so spawn distances do not change. Every model's LOD distances and
// the entity visibility test scale with it. More models drawn means more of the frame's packet buffer (0x520F90, two
// halves of 0xE6670 bytes); with 0x12BFF bytes or less free the game drops world sectors and the effects pass without
// a word, so the packet guard steps the scale back towards 1 while a frame comes near that.
//
// 100% writes the game's own values: the original game. VCS_WORLD_DETAIL_HOOKS=0 writes nothing at all.
#include "game/vcs_world_detail.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::world
{
    namespace
    {
        constexpr uint32_t kCarFactor = 0x0048A680u; // float 0.7 (FUN_00430290)
        constexpr uint32_t kPedFactor = 0x0048A67Cu; // float 0.4 (FUN_002B17A8)
        constexpr uint32_t kCarCap = 0x0048A9A8u;    // int 16: random cars + the other car counters
        constexpr uint32_t kPedCapOut = 0x00488004u; // int 25: peds outdoors (CGame::currArea 0)
        constexpr uint32_t kPedCapIn = 0x00488008u;  // int 30: peds elsewhere
        constexpr uint32_t kLodScale = 0x004887F8u;  // float 1.0: LODDistMultiplier's last factor
        constexpr uint32_t kSpawnChance = 0x00487498u; // u8[4] {0, 4, 7, 15}: spawn when rand & 15 <= [road class]
        constexpr uint32_t kCarSpawner = 0x00430290u;
        constexpr uint32_t kPedSpawner = 0x002B17A8u;
        constexpr uint32_t kPedPool = 0x00487A94u;
        constexpr uint32_t kVehiclePool = 0x00487A98u;
        constexpr uint32_t kPackets = 0x00520F90u; // +0x14 half in use, +0x18 cursor up, +0x1C data down, halves from +0x20
        constexpr uint32_t kPacketHalf = 0xE6670u;
        constexpr uint32_t kPacketDrop = 0x12BFFu; // the game skips draws with this much free or less

        // the limits (measured: docs/PERFORMANCE.md, tools/world_check.py)
        constexpr int kMaxPercent = 300;
        constexpr int kCarCapMax = 28;      // FUN_00430148 stops at 30 random cars on its own
        constexpr int kPedCapMax = 45;
        constexpr int kPedFreeMin = 16;     // pool guard: slots kept free for the scripts (drivers come from the ped pool)
        constexpr int kVehicleFreeMin = 8;
        constexpr uint32_t kPacketThrottle = kPacketDrop + 0x20000u; // free below this: step the LOD scale back
        constexpr uint32_t kPacketRecover = kPacketDrop + 0x38000u;  // free above this: let it grow again

        bool g_enabled = false;
        std::atomic<int> g_traffic{100}, g_peds{100}, g_draw{100};

        struct Original
        {
            bool read = false;
            bool valid = false;
            float carFactor = 0, pedFactor = 0, lodScale = 0;
            int carCap = 0, pedCapOut = 0, pedCapIn = 0;
            uint32_t spawnChance = 0; // the four class bytes
        } g_orig;

        // the spawn-chance table for each spawner (game thread; 0 until the first frame read the original)
        uint32_t g_carChance = 0, g_pedChance = 0;
        PS2Runtime::RecompiledFunction g_origCarSpawner = nullptr, g_origPedSpawner = nullptr;

        // game-thread state, published for the command
        float g_lodEffective = 1.0f;
        int g_holdFrames = 0;
        std::atomic<int> g_stCarCap{0}, g_stPedCapOut{0}, g_stPedCapIn{0}, g_stPedFree{-1}, g_stVehicleFree{-1},
            g_stPedsUsed{-1}, g_stVehiclesUsed{-1}, g_stGuard{0};
        std::atomic<float> g_stLod{1.0f};
        std::atomic<uint32_t> g_stPacketUsed{0}, g_stPacketPeak{0};
        std::atomic<uint64_t> g_stThrottles{0}, g_stGuardFrames{0};
        std::atomic<uint32_t> g_stCarChance{0}, g_stPedChance{0};

        // VCS_FAR_CLIP: the far plane (and so the fog end) at this distance instead of 2000 (outdoors only)
        float g_farClip = 0.0f;
        PS2Runtime::RecompiledFunction g_origSetFar = nullptr, g_origProjection = nullptr;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v)
        {
            if (rd32(ram, a) != v)
                std::memcpy(ram + (a & 0x01FFFFFFu), &v, 4);
        }
        void wrf(uint8_t *ram, uint32_t a, float f)
        {
            uint32_t v;
            std::memcpy(&v, &f, 4);
            wr32(ram, a, v);
        }

        // free entries of a CPool (+0 entries, +4 flag bytes - bit 0x80 free -, +8 size); -1 before it exists
        int poolFree(const uint8_t *ram, uint32_t poolPtr, int &used)
        {
            used = -1;
            const uint32_t pool = rd32(ram, poolPtr);
            if (!pool)
                return -1;
            const uint32_t flags = rd32(ram, pool + 4);
            const int size = static_cast<int>(rd32(ram, pool + 8));
            if (!flags || size <= 0 || size > 4096)
                return -1;
            int spare = 0;
            const uint8_t *f = ram + (flags & 0x01FFFFFFu);
            for (int i = 0; i < size; ++i)
                spare += (f[i] & 0x80) ? 1 : 0;
            used = size - spare;
            return spare;
        }

        int scaledCap(int original, int percent, int limit)
        {
            if (percent <= 100)
                return original;
            return std::clamp(static_cast<int>(std::lround(original * percent / 100.0)), original, std::max(original, limit));
        }

        void readOriginals(const uint8_t *ram)
        {
            g_orig.read = true;
            g_orig.carFactor = rdf(ram, kCarFactor);
            g_orig.pedFactor = rdf(ram, kPedFactor);
            g_orig.lodScale = rdf(ram, kLodScale);
            g_orig.carCap = static_cast<int>(rd32(ram, kCarCap));
            g_orig.pedCapOut = static_cast<int>(rd32(ram, kPedCapOut));
            g_orig.pedCapIn = static_cast<int>(rd32(ram, kPedCapIn));
            g_orig.spawnChance = rd32(ram, kSpawnChance);
            // the executable's values (docs/game-internals.md 12d); anything else: another build, leave it alone
            g_orig.valid = g_orig.carFactor == 0.7f && g_orig.pedFactor == 0.4f && g_orig.lodScale == 1.0f &&
                           g_orig.carCap == 16 && g_orig.pedCapOut == 25 && g_orig.pedCapIn == 30 &&
                           g_orig.spawnChance == 0x0F070400u;
            if (!g_orig.valid)
                std::fprintf(stderr,
                             "[world] unexpected spawner / LOD values (car %g cap %d, ped %g caps %d/%d, lod %g): density "
                             "and draw distance left alone\n",
                             g_orig.carFactor, g_orig.carCap, g_orig.pedFactor, g_orig.pedCapOut, g_orig.pedCapIn,
                             g_orig.lodScale);
        }

        // the spawn-chance table at `percent`: each class's chance ((value + 1) / 16) times it, class 0 as it is
        uint32_t scaledChance(uint32_t original, int percent)
        {
            if (percent <= 100)
                return original;
            uint32_t out = original & 0xFFu;
            for (int c = 1; c < 4; ++c)
            {
                const int v = static_cast<int>((original >> (c * 8)) & 0xFFu);
                const int scaled = std::clamp(static_cast<int>(std::lround((v + 1) * percent / 100.0)) - 1, v, 15);
                out |= static_cast<uint32_t>(scaled) << (c * 8);
            }
            return out;
        }

        PS2_GUEST_CC void carSpawner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_carChance)
                wr32(rdram, kSpawnChance, g_carChance);
            g_origCarSpawner(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void pedSpawner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_pedChance)
                wr32(rdram, kSpawnChance, g_pedChance);
            g_origPedSpawner(rdram, ctx, runtime);
        }

        // bytes used in the packet buffer's current half (FUN_00476770): the frame being kicked
        uint32_t packetUse(const uint8_t *ram)
        {
            const uint32_t half = rd32(ram, kPackets + 0x14);
            if (half > 1)
                return 0;
            const uint32_t start = kPackets + 0x20 + half * kPacketHalf;
            const uint32_t cursor = rd32(ram, kPackets + 0x18) & 0x01FFFFFFu;
            const uint32_t data = rd32(ram, kPackets + 0x1C) & 0x01FFFFFFu;
            const uint32_t end = start + kPacketHalf;
            if (cursor < start - 0x10 || cursor > end || data < start || data > end || cursor > data + 0x10)
                return 0;
            return ((cursor - (start - 0x10)) & ~0xFu) + (end - data);
        }

        // RwCameraSetFarClipPlane (FUN_0018F400: a0 camera, f12 far) from the per-frame camera setup (0x21EBB0)
        PS2_GUEST_CC void setFarClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == 0x0021EBB8u && ctx->f[12] == 2000.0f)
                ctx->f[12] = g_farClip;
            g_origSetFar(rdram, ctx, runtime);
        }

        // the projection / clip planes (FUN_002F57A0: f12 tan, f13 aspect, f14 near, f15 far = the constant 2000) from
        // RwCameraBeginUpdate (0x18F088, $s1 = the camera): the camera's own far when we moved it
        PS2_GUEST_CC void setProjection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == 0x0018F090u && rdf(rdram, GPR_U32(ctx, 17) + 0x7C) == g_farClip)
                ctx->f[15] = g_farClip;
            g_origProjection(rdram, ctx, runtime);
        }

        settings::Spec percentSpec(const char *key, const char *label, const char *help, const char *env, std::atomic<int> &target)
        {
            settings::Spec s;
            s.key = key;
            s.page = "Enhancements";
            s.section = "World";
            s.label = label;
            s.help = help;
            s.type = settings::Type::Integer;
            s.min = 100;
            s.max = kMaxPercent;
            s.format = "%d%%";
            s.defaultValue = 100;
            s.env = env;
            s.onChange = [&target](int v) { target.store(v, std::memory_order_relaxed); };
            return s;
        }
    }

    void registerSettings()
    {
        settings::add(percentSpec("enhance.traffic", "Traffic density",
                                  "How many cars drive around the city. 100% is the original game; missions that clear "
                                  "the streets still do. Costs some CPU time.",
                                  "VCS_TRAFFIC", g_traffic));
        settings::add(percentSpec("enhance.peds", "Pedestrian density",
                                  "How many people walk the streets. 100% is the original game; missions that clear "
                                  "the streets still do. Costs some CPU time.",
                                  "VCS_PEDS", g_peds));
        settings::add(percentSpec("enhance.draw_distance", "Draw distance",
                                  "How far away buildings, props and detailed models still show. 100% is the original "
                                  "game. Steps back on its own where the PS2's draw budget would run out. Costs GPU time.",
                                  "VCS_DRAW_DISTANCE", g_draw));
    }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_WORLD_DETAIL_HOOKS");
        g_enabled = !(env && env[0] == '0');
        if (!g_enabled)
            std::fprintf(stderr, "[world] density and draw distance left alone (VCS_WORLD_DETAIL_HOOKS=0)\n");
        if (g_enabled)
        {
            g_origCarSpawner = runtime.lookupFunction(kCarSpawner);
            g_origPedSpawner = runtime.lookupFunction(kPedSpawner);
            if (g_origCarSpawner && g_origPedSpawner)
            {
                runtime.replaceFunction(kCarSpawner, &carSpawner);
                runtime.replaceFunction(kPedSpawner, &pedSpawner);
            }
            else
                std::fprintf(stderr, "[world] the spawners 0x%x / 0x%x not found: density by the limits only\n", kCarSpawner,
                             kPedSpawner);
        }

        if (const char *far = std::getenv("VCS_FAR_CLIP"); g_enabled && far && *far)
        {
            const float metres = static_cast<float>(std::atof(far));
            g_origSetFar = runtime.lookupFunction(0x0018F400u);
            g_origProjection = runtime.lookupFunction(0x002F57A0u);
            if (metres > 2000.0f && metres <= 20000.0f && g_origSetFar && g_origProjection)
            {
                g_farClip = metres;
                runtime.replaceFunction(0x0018F400u, &setFarClip);
                runtime.replaceFunction(0x002F57A0u, &setProjection);
                std::fprintf(stderr, "[world] far plane and fog end at %.0f instead of 2000 (VCS_FAR_CLIP)\n", metres);
            }
            else
                std::fprintf(stderr, "[world] VCS_FAR_CLIP=%s ignored (2000 < metres <= 20000)\n", far);
        }

        // worlddetail -> ok traffic=.. peds=.. draw=.. carcap=.. pedcap=out/in lod=.. guard=0|1 pedfree=.. vehfree=..
        //   peds=used vehicles=used packets=<bytes, last frame> peak=<bytes since the last query> throttles=.. far=..
        ps2control::addCommand("worlddetail", [](const std::string &, ps2control::Reply reply) {
            char text[512];
            std::snprintf(text, sizeof(text),
                          "ok enabled=%d valid=%d traffic=%d peds=%d draw=%d carcap=%d pedcap=%d/%d lod=%.3f guard=%d "
                          "guardframes=%llu pedfree=%d vehfree=%d pedsused=%d vehused=%d packets=%u peak=%u throttles=%llu "
                          "far=%.0f carchance=%08x pedchance=%08x",
                          g_enabled ? 1 : 0, g_orig.valid ? 1 : 0, g_traffic.load(), g_peds.load(), g_draw.load(),
                          g_stCarCap.load(), g_stPedCapOut.load(), g_stPedCapIn.load(), g_stLod.load(), g_stGuard.load(),
                          (unsigned long long)g_stGuardFrames.load(), g_stPedFree.load(), g_stVehicleFree.load(),
                          g_stPedsUsed.load(), g_stVehiclesUsed.load(), g_stPacketUsed.load(), g_stPacketPeak.exchange(0),
                          (unsigned long long)g_stThrottles.load(), g_farClip > 0 ? g_farClip : 2000.0f, g_stCarChance.load(),
                          g_stPedChance.load());
            reply(text);
        });
    }

    void onGameFrame(uint8_t *rdram)
    {
        if (!g_enabled)
            return;
        if (!g_orig.read)
            readOriginals(rdram);
        if (!g_orig.valid)
            return;

        // ---- density
        const int traffic = std::clamp(g_traffic.load(std::memory_order_relaxed), 100, kMaxPercent);
        const int peds = std::clamp(g_peds.load(std::memory_order_relaxed), 100, kMaxPercent);
        int pedsUsed = -1, vehiclesUsed = -1;
        const int pedFree = poolFree(rdram, kPedPool, pedsUsed);
        const int vehicleFree = poolFree(rdram, kVehiclePool, vehiclesUsed);
        const bool guard = (pedFree >= 0 && pedFree < kPedFreeMin) || (vehicleFree >= 0 && vehicleFree < kVehicleFreeMin);
        const int carCap = guard ? g_orig.carCap : scaledCap(g_orig.carCap, traffic, kCarCapMax);
        const int pedCapOut = guard ? g_orig.pedCapOut : scaledCap(g_orig.pedCapOut, peds, kPedCapMax);
        const int pedCapIn = guard ? g_orig.pedCapIn : scaledCap(g_orig.pedCapIn, peds, kPedCapMax);
        wrf(rdram, kCarFactor, traffic == 100 ? g_orig.carFactor : g_orig.carFactor * (traffic / 100.0f));
        wrf(rdram, kPedFactor, peds == 100 ? g_orig.pedFactor : g_orig.pedFactor * (peds / 100.0f));
        wr32(rdram, kCarCap, static_cast<uint32_t>(carCap));
        wr32(rdram, kPedCapOut, static_cast<uint32_t>(pedCapOut));
        wr32(rdram, kPedCapIn, static_cast<uint32_t>(pedCapIn));
        g_carChance = scaledChance(g_orig.spawnChance, traffic);
        g_pedChance = scaledChance(g_orig.spawnChance, peds);

        // ---- draw distance, stepped back while the frame's packets come near the game's limit
        const int draw = std::clamp(g_draw.load(std::memory_order_relaxed), 100, kMaxPercent);
        const float target = draw / 100.0f;
        const uint32_t used = packetUse(rdram);
        const uint32_t spare = used ? kPacketHalf - std::min(used, kPacketHalf) : kPacketHalf;
        if (draw == 100)
        {
            g_lodEffective = 1.0f;
            g_holdFrames = 0;
        }
        else if (spare < kPacketThrottle && g_lodEffective > 1.0f)
        {
            g_lodEffective = std::max(1.0f, g_lodEffective - 0.25f);
            g_holdFrames = 120;
            g_stThrottles.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[world] lod throttled to %.2f (%u KB free in the packet buffer)\n", g_lodEffective, spare >> 10);
        }
        else if (g_holdFrames > 0)
            --g_holdFrames;
        else if (spare > kPacketRecover)
            g_lodEffective = std::min(target, g_lodEffective + 0.01f);
        g_lodEffective = std::min(g_lodEffective, target);
        wrf(rdram, kLodScale, draw == 100 ? g_orig.lodScale : g_lodEffective);

        // ---- state for the command
        g_stCarCap.store(carCap, std::memory_order_relaxed);
        g_stCarChance.store(g_carChance, std::memory_order_relaxed);
        g_stPedChance.store(g_pedChance, std::memory_order_relaxed);
        g_stPedCapOut.store(pedCapOut, std::memory_order_relaxed);
        g_stPedCapIn.store(pedCapIn, std::memory_order_relaxed);
        g_stPedFree.store(pedFree, std::memory_order_relaxed);
        g_stVehicleFree.store(vehicleFree, std::memory_order_relaxed);
        g_stPedsUsed.store(pedsUsed, std::memory_order_relaxed);
        g_stVehiclesUsed.store(vehiclesUsed, std::memory_order_relaxed);
        g_stGuard.store(guard ? 1 : 0, std::memory_order_relaxed);
        if (guard && (traffic > 100 || peds > 100))
            g_stGuardFrames.fetch_add(1, std::memory_order_relaxed);
        g_stLod.store(draw == 100 ? g_orig.lodScale : g_lodEffective, std::memory_order_relaxed);
        g_stPacketUsed.store(used, std::memory_order_relaxed);
        if (used > g_stPacketPeak.load(std::memory_order_relaxed))
            g_stPacketPeak.store(used, std::memory_order_relaxed);
    }
}
