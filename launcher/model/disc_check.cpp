#include "disc_check.h"

#include "system.h"

#include "disc/iso_image.h"
#include "disc/vcs_disc_id.h"
#include "util/sha1.h"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        DiscResult fail(const char *code, std::string message, std::string boot = {})
        {
            return {false, code, std::move(message), std::move(boot)};
        }
    }

    DiscResult checkDisc(const fs::path &path)
    {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return fail("not_found", "file not found: " + pathText(path));
        const std::string ext = lower(pathText(path.extension()));
        if (ext == ".bin" || ext == ".cue" || ext == ".img" || ext == ".mdf" || ext == ".nrg")
            return fail("not_iso", "this is a " + ext + " image; the game reads .iso files only. Convert it to .iso first "
                                   "(for a PS2 DVD the .bin is usually an ISO already: try renaming it to .iso)");
        if (ext == ".chd")
            return fail("not_iso", "this is a .chd; the game reads .iso files only. Convert it with chdman: "
                                   "chdman extractdvd -i game.chd -o game.iso");
        if (ext == ".cso" || ext == ".zso")
            return fail("not_iso", "this is a compressed " + ext + "; the game reads .iso files only. Decompress it "
                                   "first (maxcso --decompress)");
        if (ext == ".zip" || ext == ".7z" || ext == ".rar" || ext == ".gz")
            return fail("not_iso", "this is an archive; extract the .iso from it first (7-Zip can open all of these)");

        vcs::IsoImage iso;
        if (!iso.open(path))
            return fail("not_iso", "not a readable ISO image (" + iso.error() + ")");
        vcs::IsoEntry cnf;
        std::string boot;
        if (iso.find("SYSTEM.CNF", cnf) && !cnf.isDirectory && cnf.size < 4096)
        {
            std::string text(cnf.size, '\0');
            if (iso.readBytes(uint64_t(cnf.lba) * vcs::IsoImage::kSectorSize, text.size(), text.data()))
                boot = vcs::disc::bootExecutable(text);
        }
        if (boot.empty())
            return fail("not_ps2", "not a PS2 game disc (no SYSTEM.CNF)");
        std::string upper = boot;
        for (char &c : upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (upper != vcs::disc::kElfName)
        {
            const char *what = vcs::disc::describeOtherRelease(upper);
            return fail("other_release",
                        std::string("this disc is ") + (*what ? what : "another game") + " (" + boot +
                            "). Only GTA: Vice City Stories for the USA (SLUS-21590) is supported",
                        boot);
        }
        vcs::IsoEntry elf;
        if (!iso.find(vcs::disc::kElfName, elf) || elf.isDirectory)
            return fail("not_ps2", std::string(vcs::disc::kElfName) + " is missing from the image (a damaged copy?)", boot);
        std::vector<uint8_t> bytes(elf.size);
        iso.readBytes(uint64_t(elf.lba) * vcs::IsoImage::kSectorSize, bytes.size(), bytes.data());
        const std::string sha1 = vcs::sha1Hex(bytes.data(), bytes.size());
        if (sha1 != vcs::disc::kElfSha1)
            return fail("unsupported_build", "an unsupported build of the game's executable (sha1 " + sha1 +
                                                 "): a modified or damaged copy?", boot);
        return {true, "ok", vcs::disc::kTitle, boot};
    }

    std::vector<fs::path> discCandidates(const fs::path &root)
    {
        std::vector<fs::path> dirs = {root, root / "build"};
        for (Folder f : {Folder::Downloads, Folder::Desktop, Folder::Documents})
            if (fs::path p = knownFolder(f); !p.empty())
                dirs.push_back(p);
        std::vector<fs::path> found;
        auto consider = [&](const fs::path &file) {
            std::error_code ec;
            if (lower(pathText(file.extension())) != ".iso" || !fs::is_regular_file(file, ec))
                return;
            const uintmax_t size = fs::file_size(file, ec);
            if (ec || size < (512ull << 20) || size > (9ull << 30)) // a DVD image (VCS is ~3.5 GB)
                return;
            if (std::find(found.begin(), found.end(), file) == found.end())
                found.push_back(file);
        };
        for (const fs::path &dir : dirs)
        {
            std::error_code ec;
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec))
            {
                if (it->is_directory(ec) && dir != root)
                {
                    std::error_code inner;
                    for (fs::directory_iterator sub(it->path(), fs::directory_options::skip_permission_denied, inner), e2;
                         !inner && sub != e2; sub.increment(inner))
                        consider(sub->path());
                }
                else
                    consider(it->path());
            }
        }
        // the likeliest first: a name that says Vice City Stories
        std::stable_sort(found.begin(), found.end(), [](const fs::path &a, const fs::path &b) {
            const bool va = lower(pathText(a.filename())).find("vice city stories") != std::string::npos;
            const bool vb = lower(pathText(b.filename())).find("vice city stories") != std::string::npos;
            return va > vb;
        });
        return found;
    }
}
