// See vcs_footcam.h and game-internals.md 12b ("On foot: the mouse's camera").
#include "game/vcs_footcam.h"

#include "game/vcs_freecam.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::footcam
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kFollowPed = 0x0029DAB0u; // the camera behind the player on foot (mode 4): a0 the CCam, a1 the
                                                     // point it turns about (the player's position)
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x50u; // byte: which of Cams[3] is active
        constexpr uint32_t kCams = kTheCamera + 0x70u, kCamSize = 0x270u;
        constexpr uint32_t kBehindPlayer = kTheCamera + 0x58u;   // the game asks for the camera right behind the player,
        constexpr uint32_t kInFrontOfPlayer = kTheCamera + 0x858u; // or in front of him: the next update starts afresh
        constexpr uint32_t kLooksAtPlayer = kTheCamera + 0x888u; // 0 under a script camera
        // the CCam: +0 mode, +0x70 the entity it follows, +0x78 alpha (pitch), +0x7C beta (heading), +0xAC set for its
        // first update, +0x130 / +0x140 the pitch / heading speeds, +0x1A0 the point the last update turned about,
        // +0x220 / +0x230 where it stands (the heading and pitch are measured from these at the next update)
        constexpr uint32_t kCamTargetEntity = 0x70u, kCamAlpha = 0x78u, kCamBeta = 0x7Cu, kCamRestart = 0xACu;
        constexpr uint32_t kCamPitchSpeed = 0x130u, kCamHeadingSpeed = 0x140u, kCamTarget = 0x1A0u;
        constexpr uint32_t kCamPoints[2] = {0x220u, 0x230u};
        constexpr uint32_t kModeFollowPed = 4;
        // the camera's tuning, outdoors and indoors (0x3C apart; indoors when 0x489F7C is neither 0 nor 4): +0x2C the
        // cap of its turn behind a walking player, a step (also of the turn L1 and a shot from the hip ask for),
        // +0x34 / +0x38 the pitch limits up / down
        constexpr uint32_t kTable = 0x00487B30u, kTableRow = 0x3Cu, kSwingCap = 0x2Cu, kPitchUp = 0x34u, kPitchDown = 0x38u;
        constexpr uint32_t kIndoors = 0x00489F7Cu;
        constexpr uint32_t kCentring = 0x00487E10u;  // set: the game is turning the camera behind the player (L1, a shot
                                                     // from the hip); it clears it when the camera is there or he moves
        constexpr uint32_t kWallSkip = 0x00487E58u;  // > 0: no five lines this update, the line of sight pulls the
                                                     // camera in instead (the game sets 2 after a pull-in, and counts down)
        constexpr uint32_t kLastAlpha = 0x00487E5Cu; // last update's alpha / beta: a change below 0.0001 is undone
        constexpr uint32_t kLastBeta = 0x00487E60u;
        constexpr uint32_t kCamStickLocked = 0x00488EB4u; // the camera ignores the stick's turn while set
        constexpr uint32_t kPad0 = 0x005147A8u;      // CPad 0: +0xA L1 (s16), +0x96 DisablePlayerControls (s16)
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed
        constexpr uint32_t kPedMoveSpeed = 0x140u;   // units a step (the same place in every physical entity)
        constexpr uint32_t kPedStandsOn = 0x514u;    // the entity he stands on (0: the ground); the game's camera takes
                                                     // his speed relative to it
        constexpr uint32_t kTimeStep = 0x004CD110u;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        constexpr uint32_t kUserPause = 0x004CD2ACu, kCodePause = 0x004CD11Cu; // bytes

        constexpr float kTwoPi = 6.2831855f;
        // The turn behind a moving player once the camera is free to: toward opposite to where he moves, eased
        // (1 - e^(-t / kFollowTime) of what is left), at most kFollowRate radians a second at a run and less in
        // proportion below it - the same curve at a walk -, coming in over kFollowRamp seconds. With the keys following
        // the camera a sideways runner keeps the gap open, so the rate is how tight he curves: 0.6 rad/s at a run's
        // 4.5 m/s is a 7.5 m radius, where the game's camera on its string turns 1.4 rad/s about itself (a 3.5 m one).
        constexpr float kFollowTime = 0.6f, kFollowRate = 0.6f, kFollowRamp = 0.8f;
        constexpr float kRunSpeed = 0.08f;        // a run, units a step (it varies 0.085 - 0.105 with his stride)
        constexpr float kMoving = 0.01f;          // the player moves: units a step (0.5 m/s; a walk is 0.03)
        constexpr float kBehind = 0.01f;          // radians: the camera is behind him (his track's small sway is not
                                                  // the camera's)
        constexpr float kSwingLimit = 2.9670599f; // the game's own turn leaves a camera further than this (170 degrees)
                                                  // from behind him where it is: running at the camera
        constexpr float kLong = 1.0e6f;           // seconds: "long ago"

        uint8_t *g_rdram = nullptr;
        PS2Runtime::RecompiledFunction g_orig = nullptr;
        std::atomic<bool> g_installed{false};
        std::atomic<float> g_delay{1.5f};

        // game thread
        enum class State
        {
            Idle,   // the game's camera (the gamepad, L1, the camera keys, a shot from the hip; not on foot)
            Hold,   // the mouse moved or the movement keys changed: the camera stays where it is put
            Follow, // ... and since then long enough: it turns behind him while he moves
        };
        State g_state = State::Idle;
        std::atomic<int> g_stateShown{0};
        bool g_enabled = false;             // the keyboard / mouse is in use, Mouse camera on
        float g_turn = 0.0f, g_tilt = 0.0f; // the mouse since the last update
        bool g_stick = false;               // the right stick since the last update
        int g_keys = 0, g_heldKeys = 0;     // the movement keys held now (walkKeys), at the last update
        uint32_t g_frame = 0;               // the last camera update's frame
        float g_sinceInput = kLong, g_sinceMouse = kLong; // seconds of game time since the mouse or the keys, the mouse
        float g_following = 0.0f;                         // ... of turning behind him
        float g_lastTarget[3] = {};
        bool g_swingOff = false;
        float g_swingCaps[2] = {};

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        int16_t rd16(uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 2);
            return v;
        }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }
        void wrf(uint32_t a, float v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }

        float wrap(float a)
        {
            a = std::fmod(a, kTwoPi);
            return a < 0.0f ? a + kTwoPi : a;
        }

        void setState(State s)
        {
            g_state = s;
            g_stateShown.store(static_cast<int>(s), std::memory_order_relaxed);
        }

        // The game's own turn behind a walking player: off while this camera has the view, the game's otherwise.
        void swingOff(bool off)
        {
            if (off == g_swingOff)
                return;
            for (int i = 0; i < 2; ++i)
            {
                const uint32_t at = kTable + i * kTableRow + kSwingCap;
                if (off)
                    g_swingCaps[i] = rdf(at);
                wrf(at, off ? 0.0f : g_swingCaps[i]);
            }
            g_swingOff = off;
        }

        // The game's camera, from the next update on.
        void reset()
        {
            swingOff(false);
            setState(State::Idle);
            g_following = 0.0f;
        }

        // Turns the camera about the point it looks at: `turn` about z (left +), `tilt` up / down (up +, kept in the
        // game's limits). Its heading and pitch, the last update's (or the game would undo a small change), and the
        // source points the next update measures them from.
        void turnCamera(uint32_t cam, const float p[3], float turn, float tilt)
        {
            if (turn != 0.0f)
            {
                wrf(cam + kCamBeta, wrap(rdf(cam + kCamBeta) + turn));
                wrf(kLastBeta, wrap(rdf(kLastBeta) + turn));
                const float c = std::cos(turn), s = std::sin(turn);
                for (uint32_t off : kCamPoints)
                {
                    const uint32_t at = cam + off;
                    const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1];
                    wrf(at, p[0] + ox * c - oy * s);
                    wrf(at + 4, p[1] + ox * s + oy * c);
                }
            }
            const uint32_t indoors = rd32(kIndoors);
            const uint32_t table = kTable + (indoors != 0 && indoors != 4 ? kTableRow : 0u);
            const float alpha = rdf(cam + kCamAlpha);
            tilt = std::clamp(alpha + tilt, -rdf(table + kPitchDown), rdf(table + kPitchUp)) - alpha;
            if (tilt != 0.0f)
            {
                wrf(cam + kCamAlpha, alpha + tilt);
                wrf(kLastAlpha, rdf(kLastAlpha) + tilt);
                for (uint32_t off : kCamPoints)
                {
                    const uint32_t at = cam + off;
                    const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1], oz = rdf(at + 8) - p[2];
                    const float h = std::sqrt(ox * ox + oy * oy), d = std::sqrt(h * h + oz * oz);
                    if (h < 1e-4f)
                        continue;
                    // looking up is a source point below the target
                    const float e = std::clamp(std::atan2(-oz, h) + tilt, -1.55f, 1.55f);
                    const float k = d * std::cos(e) / h;
                    wrf(at, p[0] + ox * k);
                    wrf(at + 4, p[1] + oy * k);
                    wrf(at + 8, p[2] - d * std::sin(e));
                }
            }
        }

        // Before the game's update of the camera behind the player on foot (he has moved this frame).
        void prepare(uint32_t cam, uint32_t target)
        {
            const bool stick = g_stick;
            float turn = g_turn, tilt = g_tilt;
            g_turn = g_tilt = 0.0f;
            g_stick = false;
            const bool keysChanged = g_keys != g_heldKeys;
            g_heldKeys = g_keys;

            // the trainer's free camera has the view (and writes the CCam's target point): a fresh start after it
            if (freecam::flying())
                return reset();
            const uint32_t frame = rd32(kFrameCounter);
            const bool gap = frame - g_frame > 1; // a frame without this camera (a car, an aim, a cutscene)
            g_frame = frame;
            if (gap)
                turn = tilt = 0.0f; // the mouse's from before it: not this camera's (the aim's hand-back sets its heading)
            // the point it turns about this frame, and how far that moved since the last update. The update lifts the
            // point by the table's height and, on some ground, further: the last one's, moved along, is what the
            // source points stand around
            float p[3], moved[3];
            for (int i = 0; i < 3; ++i)
            {
                p[i] = rdf(target + i * 4);
                moved[i] = gap ? 0.0f : p[i] - g_lastTarget[i];
                g_lastTarget[i] = p[i];
            }
            if (!gap)
                p[2] = rdf(cam + kCamTarget + 8) + moved[2];

            const float dt = std::clamp(rdf(kTimeStep), 0.0f, 5.0f) / 50.0f;
            const float delay = g_delay.load(std::memory_order_relaxed);
            const bool mouseMoved = turn != 0.0f || tilt != 0.0f;
            g_sinceMouse = mouseMoved ? 0.0f : std::min(g_sinceMouse + dt, kLong);
            g_sinceInput = mouseMoved || keysChanged ? 0.0f : std::min(g_sinceInput + dt, kLong);
            const bool mouseHas = delay < 0.0f || g_sinceMouse < delay; // the mouse has the camera to itself

            // The game's own camera: no follow camera behind the player (another camera, a restart, a script's), its
            // stick locked or the controls off, paused, L1 held (the player asks the game to put the camera behind
            // him), the right stick - and the game turning it behind him for a shot from the hip, unless the mouse
            // has just said otherwise
            const uint32_t ped = rd32(kPlayerPed);
            const bool usable = g_enabled && cam == kCams + g_rdram[kActiveCam] * kCamSize &&
                                (rd32(cam) & 0xFFFFu) == kModeFollowPed && ped && rd32(cam + kCamTargetEntity) == ped &&
                                rd32(cam + kCamRestart) == 0 && rd32(kBehindPlayer) == 0 && rd32(kInFrontOfPlayer) == 0 &&
                                rd32(kLooksAtPlayer) == 1 && rd16(kPad0 + 0x96u) == 0 && rd32(kCamStickLocked) == 0 &&
                                !g_rdram[kUserPause] && !g_rdram[kCodePause] && rd16(kPad0 + 0xAu) == 0 && !stick &&
                                (rd32(kCentring) == 0 || mouseHas);
            if (!usable)
                return reset();

            const bool hold = delay < 0.0f || g_sinceInput < delay;
            if (hold || g_state != State::Follow)
                g_following = 0.0f;
            setState(hold ? State::Hold : State::Follow);

            // This camera goes only where it is turned here: the source points move with the player (so he does not
            // drag it after him, as on a string), the game's own turns are off and the speeds left from before stop.
            // Walls: the game's path for a turned stick, the pull-in toward the player (with the stick at rest it
            // turns the camera away from them, or leaves it standing at the wall's edge)
            swingOff(true);
            for (uint32_t off : kCamPoints)
                for (int i = 0; i < 3; ++i)
                    wrf(cam + off + i * 4, rdf(cam + off + i * 4) + moved[i]);
            wrf(cam + kCamHeadingSpeed, 0.0f);
            wrf(cam + kCamPitchSpeed, 0.0f);
            wr32(kCentring, 0);
            if (rd32(kWallSkip) == 0)
                wr32(kWallSkip, 1);

            if (mouseMoved)
                turnCamera(cam, p, turn, tilt);
            if (hold)
                return;
            // free to turn behind him: opposite to where he moves (beta is the angle of the target-to-camera
            // direction), relative to what he stands on as the game's camera takes it. Not while he stands, and not
            // when he runs at the camera - the keys follow the camera, so that turn would never get there
            float vx = rdf(ped + kPedMoveSpeed), vy = rdf(ped + kPedMoveSpeed + 4);
            if (const uint32_t ground = rd32(ped + kPedStandsOn))
            {
                vx -= rdf(ground + kPedMoveSpeed);
                vy -= rdf(ground + kPedMoveSpeed + 4);
            }
            const float speed = std::hypot(vx, vy);
            const float away = std::remainder(std::atan2(-vy, -vx) - rdf(cam + kCamBeta), kTwoPi);
            if (speed <= kMoving || std::fabs(away) > kSwingLimit)
            {
                g_following = 0.0f;
                return;
            }
            if (std::fabs(away) < kBehind)
                return;
            g_following += dt;
            const float rate = kFollowRate * std::min(speed / kRunSpeed, 1.0f) * std::min(g_following / kFollowRamp, 1.0f);
            const float cap = rate * dt;
            turnCamera(cam, p, std::clamp(away * (1.0f - std::exp(-dt / kFollowTime)), -cap, cap), 0.0f);
        }

        // FUN_0029DAB0 (CCam::Process's case for mode 4): the mouse's say first, then the game's update.
        PS2_GUEST_CC void followPed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            prepare(GPR_U32(ctx, 4), GPR_U32(ctx, 5));
            g_orig(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *e = std::getenv("VCS_FOOTCAM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[footcam] off (VCS_FOOTCAM=0)\n");
            return;
        }
        g_orig = runtime.lookupFunction(kFollowPed);
        if (!g_orig)
            return;
        runtime.replaceFunction(kFollowPed, &followPed);
        g_installed = true;
        // footcam -> ok state=<idle|hold|follow> (tools/controls_check.py)
        ps2control::addCommand("footcam", [](const std::string &, ps2control::Reply reply) {
            static const char *const names[] = {"idle", "hold", "follow"};
            reply(std::string("ok state=") + names[g_stateShown.load(std::memory_order_relaxed)]);
        });
    }

    bool active() { return g_installed.load(std::memory_order_relaxed); }

    void enable(bool on) { g_enabled = on; }

    void mouse(float turn, float tilt)
    {
        g_turn += turn;
        g_tilt += tilt;
    }

    void walkKeys(int direction) { g_keys = direction; }

    void stickMoved() { g_stick = true; }

    void setReturnDelay(float seconds) { g_delay.store(seconds, std::memory_order_relaxed); }
}
