// The one supported disc: what identifies it, shared by vcs.exe (src/main.cpp) and the launcher (launcher/), so the
// two can't disagree. tools/vcs_disc.py holds the same values for the build (KNOWN_BUILDS).
#pragma once

#include <string>
#include <string_view>

namespace vcs::disc
{
#ifdef VCS_EXPERIMENTAL_PAL
    constexpr const char *kElfName = "SLES_546.22";
    constexpr const char *kElfSha1 = "47048caa4bd2ce8bd5223847b7abe02aa790bc3e";
    constexpr const char *kTitle = "GTA: Vice City Stories - Europe (SLES-54622)";
#else
    constexpr const char *kElfName = "SLUS_215.90";
    constexpr const char *kElfSha1 = "d1832508caf0990c2291c0f255c96a889cce9f88";
    constexpr const char *kTitle = "GTA: Vice City Stories - USA (SLUS-21590) v1.00";
#endif

    // vcs.exe's exit codes for a start that never reached the game (the launcher names the reason)
    enum ExitCode : int
    {
        kExitOk = 0,
        kExitError = 1,       // anything else (an exception, the runtime stopping)
        kExitNoDisc = 2,      // no disc image given or found
        kExitDiscUnreadable = 3,
        kExitWrongDisc = 4,   // not SLUS_215.90, or another build of it
        kExitInitFailed = 5,  // the runtime (window, renderer, sound) could not start
    };

    // A disc's boot executable named by SYSTEM.CNF ("BOOT2 = cdrom0:\SLES_546.22;1" -> "SLES_546.22"), empty when
    // there is none (not a PS2 disc).
    inline std::string bootExecutable(std::string_view systemCnf)
    {
        const size_t boot = systemCnf.find("BOOT2");
        if (boot == std::string_view::npos)
            return {};
        size_t start = systemCnf.find_first_of("\\:", boot);
        while (start != std::string_view::npos && start + 1 < systemCnf.size() &&
               (systemCnf[start] == '\\' || systemCnf[start] == ':'))
            ++start;
        if (start == std::string_view::npos)
            return {};
        size_t end = start;
        while (end < systemCnf.size() && systemCnf[end] != ';' && systemCnf[end] != '\r' && systemCnf[end] != '\n' &&
               systemCnf[end] != ' ')
            ++end;
        return std::string(systemCnf.substr(start, end - start));
    }

    // What a boot executable other than ours is, in words a player can act on (empty when unknown).
    inline const char *describeOtherRelease(std::string_view elf)
    {
        if (elf == "SLES_546.22") // the PAL build (DECISIONS.md, the BlackLine map)
            return "the European release of Vice City Stories";
        if (elf == "SLUS_214.23") // read from a disc's SYSTEM.CNF
            return "Liberty City Stories (USA), not Vice City Stories";
        return "";
    }
}
