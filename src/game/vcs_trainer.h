// The trainer (docs/TRAINER.md): god mode, unlimited ammo, wanted level, money, weapons, vehicle spawning and repair, missions,
// teleports to the city's districts, time / weather / game speed - for single-player fun, off unless used.
//
// What it does goes through the game's own script commands (MAIN.SCM opcodes, game-internals.md 11), run as a short
// script on the game's script VM between frames (vcs::rig::startCodeScript): the game applies them as its scripts
// would. Values that must hold (health under god mode, ammo, the clip under no reload, the vehicle's health) are
// written into the player's ped / vehicle at the frame kick every few frames while their toggle is on
// (game-internals.md 12). Trainer scripts start only while the game runs (its clock advances), and one still waiting
// when the game pauses is taken back out, so none can end up in a save. Names (vehicles, districts, weapons) come
// from the game's own text at run time.
//
// The menu is src/ui/trainer_menu.cpp. Control channel (tests): `trainer <command> [arg]` - see install().
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace vcs::trainer
{
    enum Toggle : int
    {
        GodMode,
        NeverWanted,
        UnlimitedAmmo,
        NoReload,
        InfiniteSprint,
        FastReload,
        PoliceIgnore,
        EveryoneIgnores,
        VehicleInvincible,
        WarpIntoSpawned,
        FreezeTime,
        FreeCamera, // game/vcs_freecam.h
        HideHud,    // CHud::Draw skipped (game/vcs_hud.cpp)
        ToggleCount
    };

    struct Entry // a list item: what the menu shows, and the id the action takes
    {
        std::string name;
        int id = 0;
    };

    // Main thread, before runtime.run(): the control-channel command.
    void install(PS2Runtime &runtime);
    // Main thread, before runtime.run(): MAIN.SCM from the disc, for the mission list (each mission's text name).
    void setMissionScript(const std::vector<uint8_t> &mainScm);
    // Game thread, once per game frame (the frame kick): queued actions, the held toggles, the name tables.
    void onGameFrame(uint8_t *rdram);

    // Any thread (the menu): state and actions. Actions are queued for the game thread's next frame kick.
    bool toggle(Toggle t);
    void setToggle(Toggle t, bool on);
    bool ready();                       // in game: the script engine runs and the player exists
    bool inVehicle();                   // the player drives a vehicle (as of the last frame kick)
    void heal();                        // full health and armour
    void addMoney(int amount);
    void setWanted(int stars);          // 0 clears it
    void giveWeapon(int type);          // one of weapons()
    void giveLoadout(int which);        // one of loadouts()
    void removeWeapons();
    void spawnVehicle(int model);       // one of vehicles(): ahead of the player, who gets in (WarpIntoSpawned)
    void repairVehicle();
    void flipVehicle();                 // the player's vehicle upright, facing where it faces
    void boostVehicle();
    void paintVehicle();                // random colours
    void teleport(int landmark);        // one of landmarks()
    void savePosition();
    void returnToSaved();
    bool hasSavedPosition();
    void teleportToCamera();            // the player to the ground below the free camera, which then ends
    int freeCameraSpeed();              // metres a second (before Shift / Ctrl)
    void setFreeCameraSpeed(int metresPerSecond);
    void setTime(int hour);
    void setWeather(int weather);       // one of weathers(); -1 lets the weather change again
    void launchMission(int index);      // one of missions(): the game's own mission start (not while one runs)
    bool missionRunning();
    void setGameSpeed(int percent);     // 25..200
    int gameSpeed();
    int clockHour();                    // the game's clock (as of the last frame kick)

    std::vector<Entry> vehicles();      // sorted by name (empty until the game's text is loaded)
    std::vector<Entry> weapons();
    std::vector<Entry> loadouts();
    std::vector<Entry> landmarks();
    std::vector<Entry> weathers();
    std::vector<Entry> missions(bool story); // story missions in story order, or the side jobs (empty until text loads)

    // Notifications for the menu: the newest last (what was done, or why not), each with the time it was posted.
    struct Notice
    {
        std::string text;
        double time = 0.0; // seconds (steady clock)
        bool screenshot = false; // from the screenshot key: left out of the next shot (ui::hideScreenshotNotices)
    };
    std::vector<Notice> notices(double maxAgeSeconds);
    double now(); // the notices' clock
    void notifyScreenshot(const std::string &text); // the screenshot key's notice (src/main.cpp); any thread
}
