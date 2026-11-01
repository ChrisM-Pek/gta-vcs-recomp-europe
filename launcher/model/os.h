// What the launcher calls things on each OS, in one place: the binaries, the file manager, folder separators in text.
#pragma once

namespace launcher::os
{
#ifdef _WIN32
    constexpr bool kWindows = true;
    constexpr const char *kGameExe = "vcs.exe";
    constexpr const char *kLauncherExe = "VCSLauncher.exe";
    constexpr const char *kFileManager = "Explorer";
    constexpr const char *kShortcutName = "GTA Vice City Stories.lnk";
    constexpr const char *kBuildGame = "build\\vcs.exe"; // as the player sees it in messages
    constexpr const char *kBuildIni = "build\\vcs.ini";
    constexpr const char *kTools = "tools\\";
#else
    constexpr bool kWindows = false;
    constexpr const char *kGameExe = "vcs";
    constexpr const char *kLauncherExe = "VCSLauncher";
    constexpr const char *kFileManager = "Files";
    constexpr const char *kShortcutName = "gta-vcs-launcher.desktop";
    constexpr const char *kBuildGame = "build/vcs";
    constexpr const char *kBuildIni = "build/vcs.ini";
    constexpr const char *kTools = "tools/";
#endif
}
