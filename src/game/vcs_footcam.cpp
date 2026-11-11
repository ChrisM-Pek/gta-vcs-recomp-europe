// See vcs_footcam.h and game-internals.md 12b ("On foot: the camera free of the player").
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
        constexpr uint32_t kFollowPed = 0x0029DAB0u;     // the camera behind the player on foot (mode 4): a0 the CCam,
                                                         // a1 the point it turns about (the player's position)
        constexpr uint32_t kPlayerControl = 0x00230640u; // CPlayerPed::ProcessControl (a0 the ped): the stick to a heading
        constexpr uint32_t kAfterWorld = 0x0036AFD0u;    // the next reader of the camera's heading, after the world's update
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x50u; // byte: which of Cams[3] is active
        constexpr uint32_t kCams = kTheCamera + 0x70u, kCamSize = 0x270u;
        constexpr uint32_t kBehindPlayer = kTheCamera + 0x58u;   // the game asks for the camera right behind the player,
        constexpr uint32_t kInFrontOfPlayer = kTheCamera + 0x858u; // or in front of him: the next update starts afresh
        constexpr uint32_t kLooksAtPlayer = kTheCamera + 0x888u; // 0 under a script camera
        // the heading the player's control turns the stick by (VC's Orientation): atan2 of the frame camera's forward,
        // written by the camera's update (FUN_0037B2C8) and read by the next frame's world
        constexpr uint32_t kOrientation = kTheCamera + 0x7E8u;
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
        constexpr uint32_t kCentring = 0x00487E10u;  // set: the game is turning the camera behind the player (L1, a shot)
        constexpr uint32_t kWallTurn = 0x00487E54u;  // the turn away from a wall the five lines found, a frame
        constexpr uint32_t kWallSkip = 0x00487E58u;  // > 0: no five lines this update, the line of sight pulls the
                                                     // camera in instead (the game sets 2 after a pull-in, and counts down)
        constexpr uint32_t kLastAlpha = 0x00487E5Cu; // last update's alpha / beta: a change below 0.0001 is undone
        constexpr uint32_t kLastBeta = 0x00487E60u;
        constexpr uint32_t kCamStickLocked = 0x00488EB4u; // the camera ignores the stick's turn while set
        constexpr uint32_t kPad0 = 0x005147A8u;      // CPad 0: +0xA L1 (s16), +0x96 DisablePlayerControls (s16)
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed
        constexpr uint32_t kPedForward = 0x10u, kPedMoveSpeed = 0x140u, kPedVehicle = 0x480u, kPedState = 0x8C4u;
        constexpr uint32_t kTimeStep = 0x004CD110u;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        constexpr uint32_t kUserPause = 0x004CD2ACu, kCodePause = 0x004CD11Cu; // bytes

        constexpr float kTwoPi = 6.2831855f;
        constexpr float kReturnTime = 0.6f;   // the way back eases out: 1 - e^(-t / this) of what is left
        constexpr float kReturnRate = 2.0f;   // ... at most this many radians a second
        constexpr float kReturnLimit = 4.0f;  // seconds; the game's own camera has it after that in any case
        constexpr float kMoving = 0.01f;      // the player moves: units a step (0.5 m/s; a walk is 0.03, a run 0.12)
        constexpr float kSwingLimit = 2.9670599f; // the game's swing leaves a camera further than this (170 degrees)
                                                  // from behind him where it is

        uint8_t *g_rdram = nullptr;
        PS2Runtime::RecompiledFunction g_origCamera = nullptr, g_origControl = nullptr, g_origAfterWorld = nullptr;
        std::atomic<bool> g_installed{false};
        std::atomic<float> g_delay{1.5f};

        // game thread
        enum class State
        {
            Idle,   // the game's camera
            Hold,   // the mouse moved: the camera stays where it is put
            Return, // easing back behind the player
        };
        State g_state = State::Idle;
        std::atomic<int> g_stateShown{0};
        bool g_enabled = false;             // the keyboard / mouse is in use, Mouse camera on
        float g_turn = 0.0f, g_tilt = 0.0f; // the mouse since the last update
        bool g_stick = false;               // the right stick since the last update
        uint32_t g_frame = 0;               // the last camera update's frame
        uint32_t g_followFrame = 0;         // ... the last one with the follow camera behind the player, not restarting
        float g_idle = 0.0f, g_returning = 0.0f, g_away = 0.0f; // seconds of game time
        float g_lastTarget[3] = {};
        bool g_swingOff = false;
        float g_swingCaps[2] = {};
        // the player's direction
        int g_keys = 0;             // the movement keys held now (walkKeys)
        int g_walkKeys = 0;         // ... the ones the heading below was taken for
        uint32_t g_walkFrame = 0;   // the last frame it was given to the player's control
        float g_walkHeading = 0.0f; // the camera's heading when they were pressed
        bool g_given = false;       // it is in the camera's place right now
        float g_realHeading = 0.0f; // ... and this is the camera's own
        std::atomic<int> g_givenShown{0};

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

        // The game's turn behind a walking player: off while the mouse has the camera, the game's own otherwise.
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

        void reset()
        {
            swingOff(false);
            setState(State::Idle);
            g_idle = g_returning = g_away = 0.0f;
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

        // The camera's own heading back in its place (the player's control had the movement keys' meanwhile).
        void giveBack()
        {
            if (!g_given)
                return;
            wrf(kOrientation, g_realHeading);
            g_given = false;
        }

        // Before the game's update of the camera behind the player on foot (he has moved this frame).
        void prepare(uint32_t cam, uint32_t target)
        {
            giveBack();
            const bool stick = g_stick;
            float turn = g_turn, tilt = g_tilt;
            g_turn = g_tilt = 0.0f;
            g_stick = false;

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

            const uint32_t ped = rd32(kPlayerPed);
            const bool follow = cam == kCams + g_rdram[kActiveCam] * kCamSize && (rd32(cam) & 0xFFFFu) == kModeFollowPed &&
                                ped && rd32(cam + kCamTargetEntity) == ped && rd32(cam + kCamRestart) == 0 &&
                                rd32(kBehindPlayer) == 0 && rd32(kInFrontOfPlayer) == 0 && rd32(kLooksAtPlayer) == 1;
            if (follow)
                g_followFrame = frame;
            // the game's own camera: its stick locked or the controls off, paused, L1 held (the player asks the game to
            // put the camera behind him), the right stick
            const bool usable = follow && g_enabled && rd16(kPad0 + 0x96u) == 0 && rd32(kCamStickLocked) == 0 &&
                                !g_rdram[kUserPause] && !g_rdram[kCodePause] && rd16(kPad0 + 0xAu) == 0 && !stick;
            if (!usable || gap)
            {
                reset();
                if (!usable)
                    return;
            }

            const float dt = std::clamp(rdf(kTimeStep), 0.0f, 5.0f) / 50.0f;
            const float delay = g_delay.load(std::memory_order_relaxed);
            const bool moving = std::hypot(rdf(ped + kPedMoveSpeed), rdf(ped + kPedMoveSpeed + 4)) > kMoving;
            // behind the player: beta is the angle of the target-to-camera direction
            const float behind = std::atan2(-rdf(ped + kPedForward + 4), -rdf(ped + kPedForward));
            float away = std::remainder(behind - rdf(cam + kCamBeta), kTwoPi);

            if (turn != 0.0f || tilt != 0.0f)
            {
                setState(State::Hold);
                g_idle = 0.0f;
            }
            else if (g_state == State::Hold)
            {
                g_idle += dt;
                if (delay >= 0.0f && g_idle >= delay)
                {
                    // standing, it stays where it is: the game's camera does not turn it either
                    setState(moving ? State::Return : State::Idle);
                    g_returning = 0.0f;
                }
            }
            else if (g_state == State::Idle)
            {
                // running at the camera (S): the game's swing leaves a camera in front of him there. After the same
                // delay it comes round as well
                g_away = moving && delay >= 0.0f && std::fabs(away) > kSwingLimit ? g_away + dt : 0.0f;
                if (g_away > 0.0f && g_away >= delay)
                {
                    setState(State::Return);
                    g_returning = 0.0f;
                }
            }
            if (g_state == State::Return && !moving)
                setState(State::Idle);

            if (g_state == State::Idle)
            {
                swingOff(false);
                return;
            }
            g_away = 0.0f;
            // held and on the way back: the source points move with the player (so he does not drag the camera after
            // him), the game's own turns are off and the speeds left from before stop - the camera goes only where it
            // is turned here. Walls: the game's path for a turned stick, the pull-in toward the player (with the stick
            // at rest it turns the camera away from them, or leaves it standing at the wall's edge)
            swingOff(true);
            for (uint32_t off : kCamPoints)
                for (int i = 0; i < 3; ++i)
                    wrf(cam + off + i * 4, rdf(cam + off + i * 4) + moved[i]);
            wrf(cam + kCamHeadingSpeed, 0.0f);
            wrf(cam + kCamPitchSpeed, 0.0f);
            wr32(kCentring, 0);
            if (rd32(kWallSkip) == 0)
                wr32(kWallSkip, 1);
            if (g_state == State::Hold)
                turnCamera(cam, p, turn, tilt);
            else
            {
                // back behind the player, easing out; then the game's camera has it. The pitch stays the mouse's
                g_returning += dt;
                const float k = 1.0f - std::exp(-dt / kReturnTime), cap = kReturnRate * dt;
                turnCamera(cam, p, std::clamp(away * k, -cap, cap), 0.0f);
                away = std::remainder(behind - rdf(cam + kCamBeta), kTwoPi);
                if (std::fabs(away) < 0.01f || g_returning >= kReturnLimit)
                    setState(State::Idle);
            }
        }

        // FUN_0029DAB0 (CCam::Process's case for mode 4): the mouse's say first, then the game's update.
        PS2_GUEST_CC void followPed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            prepare(GPR_U32(ctx, 4), GPR_U32(ctx, 5));
            g_origCamera(rdram, ctx, runtime);
        }

        // FUN_00230640 (CPlayerPed::ProcessControl, a0 the ped): his control turns the stick into a heading by the
        // camera's (heading = atan2(-x, -y) - Orientation, FUN_00232DE8; the fight control FUN_00233728 the same way).
        // While the same movement keys stay held it gets the camera's heading of the moment they were pressed, so he
        // keeps his direction whatever the camera does. Pressed, they take the camera's as it is: that frame is the
        // game's own.
        PS2_GUEST_CC void playerControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            giveBack();
            const uint32_t ped = rd32(kPlayerPed), frame = rd32(kFrameCounter);
            // on foot behind the follow camera (its last update: this frame's comes after the world), by the keyboard
            const bool on = g_enabled && g_keys != 0 && ped && GPR_U32(ctx, 4) == ped && frame - g_followFrame <= 1 &&
                            !(rd32(ped + kPedVehicle) && rd32(ped + kPedState) == 0x37);
            if (!on)
                g_walkKeys = 0;
            else
            {
                const float real = rdf(kOrientation);
                // (the camera keys turn the camera through the stick: the game's way, and he follows the camera as
                // the game has it - this frame's pad reads have set g_stick, the camera's update takes it after this)
                if (g_keys != g_walkKeys || frame - g_walkFrame > 1 || g_stick)
                {
                    g_walkKeys = g_keys;
                    g_walkHeading = real;
                }
                g_walkFrame = frame;
                if (g_walkHeading != real)
                {
                    g_realHeading = real;
                    g_given = true;
                    wrf(kOrientation, g_walkHeading);
                }
            }
            g_givenShown.store(on ? 1 : 0, std::memory_order_relaxed);
            g_origControl(rdram, ctx, runtime);
        }

        // FUN_0036AFD0: the next reader of the camera's heading after the world. It gets the camera's own.
        PS2_GUEST_CC void afterWorld(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            giveBack();
            g_origAfterWorld(rdram, ctx, runtime);
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
        g_origCamera = runtime.lookupFunction(kFollowPed);
        g_origControl = runtime.lookupFunction(kPlayerControl);
        g_origAfterWorld = runtime.lookupFunction(kAfterWorld);
        if (!g_origCamera || !g_origControl || !g_origAfterWorld)
        {
            std::fprintf(stderr, "[footcam] not installed: a function is missing\n");
            return;
        }
        runtime.replaceFunction(kFollowPed, &followPed);
        runtime.replaceFunction(kPlayerControl, &playerControl);
        runtime.replaceFunction(kAfterWorld, &afterWorld);
        g_installed = true;
        // footcam -> ok state=<idle|hold|return> keys=<0|1: the movement keys held have their own heading, not the
        // camera's> (tools/controls_check.py)
        ps2control::addCommand("footcam", [](const std::string &, ps2control::Reply reply) {
            static const char *const names[] = {"idle", "hold", "return"};
            reply(std::string("ok state=") + names[g_stateShown.load(std::memory_order_relaxed)] +
                  " keys=" + (g_givenShown.load(std::memory_order_relaxed) ? "1" : "0"));
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
