// The launcher's OS pieces on macOS (docs/LAUNCHER.md "macOS"): Finder through NSWorkspace, the user's folders, an
// IOKit power assertion while a build runs, Terminal through AppleScript. Objective-C++ with ARC.
#include "system.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        IOPMAssertionID g_awake = kIOPMNullAssertionID;

        fs::path home()
        {
            const char *h = std::getenv("HOME");
            return fs::path(h && *h ? h : "/tmp");
        }

        NSURL *fileUrl(const fs::path &p) { return [NSURL fileURLWithPath:[NSString stringWithUTF8String:pathText(p).c_str()]]; }
    }

    std::string lastErrorText(unsigned long code) { return std::strerror(int(code)); }

    fs::path exePath()
    {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return {};
        std::error_code ec;
        fs::path p = fs::canonical(fs::path(buffer.c_str()), ec);
        return ec ? fs::path(buffer.c_str()) : p;
    }

    fs::path knownFolder(Folder which)
    {
        switch (which)
        {
        case Folder::Desktop: return home() / "Desktop";
        case Folder::Downloads: return home() / "Downloads";
        case Folder::Documents: return home() / "Documents";
        case Folder::LocalAppData: return home() / "Library" / "Application Support"; // where the game keeps its saves
        }
        return home();
    }

    void openFolder(const fs::path &dir)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        @autoreleasepool
        {
            [[NSWorkspace sharedWorkspace] openURL:fileUrl(dir)];
        }
    }

    void revealFile(const fs::path &file)
    {
        @autoreleasepool
        {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ fileUrl(file) ]];
        }
    }

    void openUrl(const std::string &url)
    {
        @autoreleasepool
        {
            if (NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]])
                [[NSWorkspace sharedWorkspace] openURL:u];
        }
    }

    void openFile(const fs::path &file)
    {
        @autoreleasepool
        {
            [[NSWorkspace sharedWorkspace] openURL:fileUrl(file)];
        }
    }

    // A Finder alias would need the bookmark APIs; a symbolic link on the Desktop opens the launcher the same way
    // (Finder shows it with the app's icon).
    bool createShortcut(const fs::path &link, const fs::path &target, const fs::path &, const std::string &, std::string &error)
    {
        std::error_code ec;
        fs::remove(link, ec);
        fs::create_symlink(target, link, ec);
        if (ec)
        {
            error = "cannot make " + pathText(link) + " (" + ec.message() + ")";
            return false;
        }
        return true;
    }

    void keepAwake(bool on)
    {
        if (on && g_awake == kIOPMNullAssertionID)
            IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn,
                                        CFSTR("Building GTA: Vice City Stories"), &g_awake);
        else if (!on && g_awake != kIOPMNullAssertionID)
        {
            IOPMAssertionRelease(g_awake);
            g_awake = kIOPMNullAssertionID;
        }
    }

    void openTerminal(const std::string &command)
    {
        // Terminal.app running the command, kept open at the end
        std::string script = command + "; echo; echo 'Done. Close this window, then Check again in the launcher.'";
        std::string escaped;
        for (char c : script)
        {
            if (c == '\\' || c == '"')
                escaped += '\\';
            escaped += c;
        }
        @autoreleasepool
        {
            NSString *source = [NSString stringWithFormat:@"tell application \"Terminal\"\nactivate\ndo script \"%s\"\nend tell",
                                                          escaped.c_str()];
            NSAppleScript *as = [[NSAppleScript alloc] initWithSource:source];
            [as executeAndReturnError:nil];
        }
    }

    bool attachParentConsole() { return ::isatty(1) != 0; }

    bool redirectOutput(const fs::path &file)
    {
        const int fd = ::open(pathText(file).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return false;
        std::fflush(stdout);
        std::fflush(stderr);
        ::dup2(fd, 1);
        ::dup2(fd, 2);
        ::close(fd);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        return true;
    }

    uint64_t freeBytes(const fs::path &dir)
    {
        struct statvfs s{};
        if (::statvfs(pathText(dir).c_str(), &s) != 0)
            return 0;
        return uint64_t(s.f_bavail) * s.f_frsize;
    }

    double processCpuSeconds()
    {
        rusage u{};
        ::getrusage(RUSAGE_SELF, &u);
        auto seconds = [](const timeval &t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
        return seconds(u.ru_utime) + seconds(u.ru_stime);
    }

    unsigned long processId() { return static_cast<unsigned long>(::getpid()); }

    void closeInheritedFdsOnExec()
    {
        // no close_range on macOS: each fd up to the limit is marked (not closed: the exec-error pipe must still work)
        rlimit lim{};
        const long top = ::getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY ? long(lim.rlim_cur) : 4096;
        for (int fd = 3; fd < top && fd < 65536; ++fd)
            if (const int flags = ::fcntl(fd, F_GETFD); flags >= 0 && !(flags & FD_CLOEXEC))
                ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }

    void waitForProcessExit(unsigned long pid, double seconds)
    {
        // not our child: kqueue tells when it ends
        const int kq = ::kqueue();
        if (kq < 0)
            return;
        struct kevent change;
        EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        struct kevent event;
        timespec timeout{time_t(seconds), long((seconds - double(time_t(seconds))) * 1e9)};
        if (::kevent(kq, &change, 1, &event, 1, &timeout) < 0 && errno == ESRCH)
        {
            // already gone
        }
        ::close(kq);
    }
}
