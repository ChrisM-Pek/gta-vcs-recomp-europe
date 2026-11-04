// Vehicle physics that acts a fixed amount every frame, put on the game's time step (docs/game-internals.md 4).
//
// The PS2 game ran at 30 fps, and some vehicle physics is applied once a frame without CTimer::ms_fTimeStep: at 144 fps
// it acted 4.8 times as often per second. The cases here are scaled by the frame's step over the PS2's (50/30) when
// the frame is shorter, so they act as much per second as at 30 fps; longer frames keep the game's amount, as the PS2
// did when it dropped below 30. Only while the frame isn't the PS2's (independent timing, or original timing at 60).
//
// 1. The push from a body under a wheel. FUN_003F34F0 runs after each wheel's ProcessWheel, for cars (FUN_00152E18,
//    from CAutomobile::ProcessControl) and bikes (CBike::ProcessControl, FUN_00337208). When the player drives, the
//    vehicle moves and a ped near the driver is dead or knocked down within 1 m (horizontally) of the wheel's contact
//    point, it gives the vehicle an upward push at that wheel: ApplyTurnForce((0, 0, mass * 0.01 (* 1.5 for cars)),
//    contact point). A car's front wheels over a body pitched it up so fast at 144 fps that its rear hit the ground and
//    threw it up (3.3 m/s against 1.4 at 30 fps); a bike rolling past the rider it was stolen from stood on its nose.
//    The hook on ApplyTurnForce scales that one call's force (f14, return address 0x3F36D4).
// 2. Rolling resistance. CVehicle::ProcessWheel (FUN_003E6C18 cars, FUN_003E7618 bikes) brakes a wheel with neither
//    gas nor brake by 0.9 / mass (other constants for light vehicles, the RC Bandit and bikes) per frame: a car coasted
//    down at 16 m/s^2 at 144 fps instead of 3.4. Patched in game/vcs/overrides.toml to read those constants from guest
//    RAM 0x7FF00.. (kBrakeSlots); the ProcessWheel hooks write them, scaled, before every call - always, also with this
//    off, since the patched code has no constant of its own any more.
// 3. A bike's brake wobble (GitHub issue 64). CBike::ProcessControl (at 0x33BD18..0x33BE54), while the brake leads the
//    gas by more than 0.9, the bike rolls forwards faster than 0.02 and a wheel is on the ground: a phase (+0x5FC)
//    grows by random(0.5..1) * 0.2 * step and the bike is pushed sideways at a wheel's contact point, ApplyTurnForce(
//    min(speed, 0.2) * sin(phase) (FUN_0044A2D0) * turn mass * (0.03 with the rear wheel down, else 0.007) * right) - once a frame,
//    the step only in the phase. At 144 fps the push came 4.8 times as often per second: braking hard from speed
//    turned the bike's yaw 3.7 times as fast, it fishtailed (heading swinging -21 to +68 degrees, sideways at 19 m/s;
//    3 degrees at 30 fps), and at 240 it went down on its side. The hook scales that call's force (f12-f14, return
//    address 0x33BE5C).
//
// VCS_PHYSICS_TIMESTEP=0 keeps all three at the game's per-frame amounts.
#include "game/vcs_physics_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kApplyTurnForce = 0x00347E78u;  // CPhysical::ApplyTurnForce(f12-f14 force, f15-f17 point, a0)
        constexpr uint32_t kBodyPushReturn = 0x003F36D4u;  // FUN_003F34F0's call: the push from a body under a wheel
        constexpr uint32_t kBrakeWobbleReturn = 0x0033BE5Cu; // CBike::ProcessControl's call: the brake wobble (3.)
        constexpr uint32_t kProcessWheelCar = 0x003E6C18u; // CVehicle::ProcessWheel
        constexpr uint32_t kProcessWheelBike = 0x003E7618u;
        constexpr uint32_t kBrakeSlots = 0x0007FF00u;      // the patched ProcessWheels' rolling-resistance constants
        constexpr float kBrakeConstants[] = {0.54f, 0.09f, 0.9f, 0.18f}; // as the game loaded them (0x3F0A3D71 ...)
        constexpr uint32_t kTimeStep = 0x004CD110u;        // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u;    // CTimer::m_FrameCounter
        constexpr float kPs2Step = 50.0f / 30.0f;          // a 30 fps frame

        PS2Runtime::RecompiledFunction g_origTurnForce = nullptr, g_origWheelCar = nullptr, g_origWheelBike = nullptr;

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_PHYSICS_TIMESTEP");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        // the frame isn't the PS2's 1/30 s: independent timing, or original timing at 60 (one VBlank a frame)
        bool active() { return enabled() && (timing() == Timing::Independent || frameCap() != 30); }

        // the frame's step over the PS2's when it is shorter, else 1 (the game's own amount)
        float frameScale(const uint8_t *rdram)
        {
            if (!active())
                return 1.0f;
            float step;
            std::memcpy(&step, rdram + kTimeStep, 4);
            return step >= 0.0f && step < kPs2Step ? step / kPs2Step : 1.0f;
        }

        float rdF(const uint8_t *rdram, uint32_t addr)
        {
            float v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFu), 4);
            return v;
        }
        uint32_t rdU(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFu), 4);
            return v;
        }
        float dot3(const uint8_t *rdram, uint32_t a, uint32_t b)
        {
            return rdF(rdram, a) * rdF(rdram, b) + rdF(rdram, a + 4) * rdF(rdram, b + 4) + rdF(rdram, a + 8) * rdF(rdram, b + 8);
        }

        // VCS_TURN_LOG=<model>: every ApplyTurnForce on vehicles of that model, with its caller and what it does to the
        // turn speed about the vertical (yaw) and the vehicle's right axis (pitch, nose up > 0) - summed by caller over a
        // stretch of frames, it shows which push grows with the frame rate (how the brake wobble was found)
        void logTurnForce(const uint8_t *rdram, R5900Context *ctx)
        {
            static const int logModel = [] {
                const char *v = std::getenv("VCS_TURN_LOG");
                return v ? std::atoi(v) : 0;
            }();
            if (!logModel)
                return;
            const uint32_t veh = GPR_U32(ctx, 4);
            int16_t model;
            std::memcpy(&model, rdram + ((veh + 0x56u) & 0x1FFFFFFu), 2);
            if (model != logModel)
                return;
            auto f = [&](uint32_t off) { return rdF(rdram, veh + off); };
            float com[3]; // the centre of mass (+0xC0) in world orientation
            for (uint32_t i = 0; i < 3; ++i)
                com[i] = f(0x00 + 4 * i) * f(0xC0) + f(0x10 + 4 * i) * f(0xC4) + f(0x20 + 4 * i) * f(0xC8);
            const float px = ctx->f[15] - com[0], py = ctx->f[16] - com[1], pz = ctx->f[17] - com[2];
            const float tx = py * ctx->f[14] - pz * ctx->f[13], ty = pz * ctx->f[12] - px * ctx->f[14];
            const float tz = px * ctx->f[13] - py * ctx->f[12];
            const float turnMass = f(0xD4);
            std::fprintf(stderr, "[turn] f %u veh %x step %.3f ra %x yaw %.7f pitch %.7f\n", rdU(rdram, kFrameCounter), veh,
                         rdF(rdram, kTimeStep), GPR_U32(ctx, 31), tz / turnMass,
                         (tx * f(0x00) + ty * f(0x04) + tz * f(0x08)) / turnMass);
        }

        PS2_GUEST_CC void applyTurnForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == kBodyPushReturn)
                ctx->f[14] *= frameScale(rdram);
            else if (ra == kBrakeWobbleReturn)
            {
                const float scale = frameScale(rdram);
                ctx->f[12] *= scale;
                ctx->f[13] *= scale;
                ctx->f[14] *= scale;
            }
            logTurnForce(rdram, ctx);
            g_origTurnForce(rdram, ctx, runtime);
        }

        void writeBrakeConstants(uint8_t *rdram)
        {
            const float scale = frameScale(rdram);
            for (size_t i = 0; i < sizeof(kBrakeConstants) / sizeof(kBrakeConstants[0]); ++i)
            {
                const float v = kBrakeConstants[i] * scale; // exact at 1
                std::memcpy(rdram + kBrakeSlots + 4u * i, &v, 4);
            }
        }

        template <PS2Runtime::RecompiledFunction *Original>
        PS2_GUEST_CC void processWheel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // VCS_WHEEL_LOG=1: each bike wheel's inputs - thrust, brake and grip (adhesion * step) for the frame, the
            // contact point's speed along the wheel (forward / sideways), wheels on the ground, and the wheel's state from
            // the last frame (0 rolling, 1 spinning, 2 skidding, 3 locked; through the pointer at the caller's sp)
            static const bool log = std::getenv("VCS_WHEEL_LOG") != nullptr;
            if (log && Original == &g_origWheelBike)
            {
                const uint32_t fwd = GPR_U32(ctx, 5), right = GPR_U32(ctx, 6), speed = GPR_U32(ctx, 7);
                std::fprintf(stderr, "[wheel] f %u veh %x step %.4f thrust %.5f brake %.5f adh %.5f p4 %.3f fwd %.5f side %.5f n %u state %u\n",
                             rdU(rdram, kFrameCounter), GPR_U32(ctx, 4), rdF(rdram, kTimeStep), ctx->f[12], ctx->f[13],
                             ctx->f[14] * rdF(rdram, kTimeStep), ctx->f[15], dot3(rdram, speed, fwd),
                             dot3(rdram, speed, right), GPR_U32(ctx, 9), rdU(rdram, rdU(rdram, GPR_U32(ctx, 29))));
            }
            writeBrakeConstants(rdram);
            (*Original)(rdram, ctx, runtime);
        }
    }

    void installPhysicsTimingHooks(PS2Runtime &runtime)
    {
        g_origTurnForce = runtime.lookupFunction(kApplyTurnForce);
        g_origWheelCar = runtime.lookupFunction(kProcessWheelCar);
        g_origWheelBike = runtime.lookupFunction(kProcessWheelBike);
        if (g_origTurnForce)
            runtime.replaceFunction(kApplyTurnForce, &applyTurnForce);
        else
            std::fprintf(stderr, "[physics-timing] ApplyTurnForce 0x%x not found: the push from bodies under wheels and the bike's brake wobble stay on the frame rate\n",
                         kApplyTurnForce);
        // The patched ProcessWheels read their constants from kBrakeSlots, so these hooks are not optional.
        if (g_origWheelCar)
            runtime.replaceFunction(kProcessWheelCar, &processWheel<&g_origWheelCar>);
        if (g_origWheelBike)
            runtime.replaceFunction(kProcessWheelBike, &processWheel<&g_origWheelBike>);
        if (!g_origWheelCar || !g_origWheelBike)
            std::fprintf(stderr, "[physics-timing] ProcessWheel 0x%x / 0x%x not found: vehicles roll without resistance\n",
                         kProcessWheelCar, kProcessWheelBike);
    }
}
