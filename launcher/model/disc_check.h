// Is a file the supported disc? The check vcs.exe makes (src/disc/vcs_disc_id.h), plus the reasons a player can act
// on: another release, another game, an archive or a .bin / .chd that needs converting. tools/vcs_disc.py check is
// the same for the command line.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace launcher
{
    struct DiscResult
    {
        bool ok = false;
        std::string code;    // ok, not_found, not_iso, not_ps2, other_release, unsupported_build
        std::string message; // for the player
        std::string boot;    // SYSTEM.CNF's executable
    };

    DiscResult checkDisc(const std::filesystem::path &path);

    // .iso files where players keep them: the launcher's folder, Downloads, Desktop, Documents (and one level below
    // each), sized like a DVD image. Not checked yet.
    std::vector<std::filesystem::path> discCandidates(const std::filesystem::path &root);
}
