// Process on Linux and macOS: fork + exec. A tool leads a process group of its own, so Cancel (and closing the launcher)
// stops the whole tree - build.py -> cmake -> ninja -> clang - as the Job Object does on Windows; the game leads a
// session of its own and plays on when the launcher closes. A waiter thread blocks in waitpid and a reader thread in
// poll, and both wake the UI, so nothing polls on a timer.
#include "process.h"

#include "system.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace launcher
{
    struct Process::Exit
    {
        std::atomic<bool> exited{false};
        int status = 0;
    };

    namespace
    {
        constexpr double kKillGraceSeconds = 2.0; // SIGTERM, then SIGKILL for whatever is left of the group

        void closeFd(int &fd)
        {
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }

        // a pipe whose ends close on exec (macOS has no pipe2: the flags right after, from the UI thread that forks)
        int cloexecPipe(int fds[2])
        {
#if defined(__linux__)
            return ::pipe2(fds, O_CLOEXEC);
#else
            if (::pipe(fds) != 0)
                return -1;
            ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
            return 0;
#endif
        }
    }

    Process::~Process()
    {
        // a tool's tree still running (or a grandchild still holding the pipe): it goes with us
        if (m_pid > 0 && m_killOnClose && m_exit && (m_ownGroup ? !m_exit->exited || !m_readerDone : !m_exit->exited))
            ::kill(m_ownGroup ? -m_pid : m_pid, SIGKILL);
        if (m_reader.joinable())
        {
            if (m_stopFd >= 0)
            {
                const uint64_t one = 1;
                [[maybe_unused]] ssize_t n = ::write(m_stopFd, &one, sizeof(one));
            }
            m_reader.join();
        }
        if (m_waiter.joinable())
        {
            if (m_exit && m_exit->exited)
                m_waiter.join();
            else
                m_waiter.detach(); // the game plays on; the waiter holds its own reference to what it writes
        }
        closeFd(m_readFd);
        if (m_stopReadFd != m_stopFd)
            closeFd(m_stopReadFd);
        m_stopReadFd = -1;
        closeFd(m_stopFd);
    }

    bool Process::start(const std::vector<std::string> &argv, const Options &options, std::string &error)
    {
        if (argv.empty())
        {
            error = "nothing to start";
            return false;
        }
        m_killOnClose = options.killOnClose;
        // a tool leads its own group; the game its own session (and so group); CLI mode (inheritConsole) stays in ours,
        // so Ctrl+C in the terminal reaches the tool too
        m_ownGroup = !options.killOnClose || !options.inheritConsole;

        // everything the child needs is made before fork: after it, only async-signal-safe calls
        std::vector<char *> args;
        for (const auto &a : argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);
        std::vector<std::string> envStrings;
        for (char **e = environ; e && *e; ++e)
        {
            const std::string_view var(*e);
            bool replaced = false;
            for (const auto &[key, value] : options.env)
                if (var.size() > key.size() && var.substr(0, key.size()) == key && var[key.size()] == '=')
                    replaced = true;
            if (!replaced)
                envStrings.emplace_back(var);
        }
        for (const auto &[key, value] : options.env)
            envStrings.push_back(key + "=" + value);
        std::vector<char *> envp;
        for (auto &e : envStrings)
            envp.push_back(e.data());
        envp.push_back(nullptr);
        const std::string cwd = options.cwd.empty() ? std::string() : pathText(options.cwd);

        int outFd = -1, readEnd = -1, nul = -1;
        if (!options.inheritConsole)
        {
            nul = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (!options.outputFile.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(options.outputFile.parent_path(), ec);
                outFd = ::open(pathText(options.outputFile).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (outFd < 0)
                {
                    error = "cannot write " + pathText(options.outputFile) + " (" + lastErrorText(errno) + ")";
                    closeFd(nul);
                    return false;
                }
            }
            else
            {
                int fds[2];
                if (cloexecPipe(fds) != 0)
                {
                    error = "pipe failed (" + lastErrorText(errno) + ")";
                    closeFd(nul);
                    return false;
                }
                readEnd = fds[0];
                outFd = fds[1];
            }
        }
        // exec's errno comes back through this pipe; it closes on a successful exec
        int status[2];
        if (cloexecPipe(status) != 0)
        {
            error = "pipe failed (" + lastErrorText(errno) + ")";
            closeFd(nul);
            closeFd(outFd);
            closeFd(readEnd);
            return false;
        }

        const pid_t parent = ::getpid();
        const bool ownGroup = m_ownGroup, killOnClose = options.killOnClose;
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            if (!killOnClose)
                ::setsid(); // the game: a session of its own, outliving the launcher and its terminal
            else if (ownGroup)
            {
                ::setpgid(0, 0);
#if defined(__linux__)
                // the launcher dying (a crash, kill) stops the tool too. Tied to the thread that forked: tools are
                // only started from the UI thread. macOS has no such signal: ~Process stops the group
                ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
                if (::getppid() != parent)
                    _exit(127);
            }
            if (nul >= 0)
                ::dup2(nul, 0);
            if (outFd >= 0)
            {
                ::dup2(outFd, 1);
                ::dup2(outFd, 2);
            }
            // the launcher's signal mask and handlers are not the tool's business
            sigset_t none;
            sigemptyset(&none);
            ::sigprocmask(SIG_SETMASK, &none, nullptr);
            ::signal(SIGPIPE, SIG_DFL);
            closeInheritedFdsOnExec(); // only 0-2 (and the exec-error pipe until exec) reach the program
            int err = 0;
            if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
                err = errno;
            else
            {
#if defined(__linux__)
                ::execvpe(args[0], args.data(), envp.data());
#else
                environ = envp.data(); // the child's own: execvp searches its PATH
                ::execvp(args[0], args.data());
#endif
                err = errno;
            }
            [[maybe_unused]] ssize_t n = ::write(status[1], &err, sizeof(err));
            _exit(127);
        }
        ::close(status[1]);
        closeFd(nul);
        closeFd(outFd);
        if (pid < 0)
        {
            error = "cannot start " + argv[0] + " (" + lastErrorText(errno) + ")";
            ::close(status[0]);
            closeFd(readEnd);
            return false;
        }
        if (ownGroup && killOnClose)
            ::setpgid(pid, pid); // also from here: no window in which kill(-pid) would miss
        int childErr = 0;
        ssize_t got;
        do
            got = ::read(status[0], &childErr, sizeof(childErr));
        while (got < 0 && errno == EINTR);
        ::close(status[0]);
        if (got == sizeof(childErr))
        {
            int ignored;
            ::waitpid(pid, &ignored, 0);
            closeFd(readEnd);
            error = "cannot start " + argv[0] + " (" + lastErrorText(unsigned(childErr)) + ")";
            return false;
        }
        m_pid = pid;

        m_exit = std::make_shared<Exit>();
        m_waiter = std::thread([pid, exit = m_exit] {
            int st = 0;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR)
            {
            }
            exit->status = st;
            exit->exited = true;
            Process::wakeUi();
        });

        if (readEnd >= 0)
        {
            m_readFd = readEnd;
#if defined(__linux__)
            m_stopFd = m_stopReadFd = ::eventfd(0, EFD_CLOEXEC);
#else
            int stop[2];
            if (cloexecPipe(stop) == 0)
            {
                m_stopReadFd = stop[0];
                m_stopFd = stop[1];
            }
#endif
            m_readerDone = false;
            m_reader = std::thread([this] {
                char buffer[4096];
                pollfd fds[2] = {{m_readFd, POLLIN, 0}, {m_stopReadFd, POLLIN, 0}};
                for (;;)
                {
                    if (::poll(fds, m_stopReadFd >= 0 ? 2 : 1, -1) < 0)
                    {
                        if (errno == EINTR)
                            continue;
                        break;
                    }
                    if (fds[1].revents)
                        break;
                    const ssize_t n = ::read(m_readFd, buffer, sizeof(buffer));
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n <= 0)
                        break;
                    consume(buffer, size_t(n));
                }
                finishReading();
            });
        }
        return true;
    }

    bool Process::running()
    {
        if (m_pid <= 0 || !m_exit)
            return false;
        if (!m_exit->exited)
            return true;
        // the rest of its output first - but not forever: a grandchild may keep the pipe open after the tool ended
        const auto now = std::chrono::steady_clock::now();
        if (m_exitSeen == std::chrono::steady_clock::time_point{})
            m_exitSeen = now;
        if (!m_readerDone && now - m_exitSeen < std::chrono::seconds(2))
            return true;
        const int st = m_exit->status;
        m_exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
        return false;
    }

    int Process::exitCode()
    {
        running();
        return m_exitCode;
    }

    void Process::kill()
    {
        if (m_pid <= 0)
            return;
        const pid_t target = m_ownGroup ? -m_pid : m_pid;
        ::kill(target, SIGTERM);
        // whatever ignores SIGTERM goes after a grace period. A group that is gone by then answers ESRCH (its id
        // can't be reused while a member lives); a lone process is only signalled while it hasn't been reaped
        std::thread([target, exit = m_exit] {
            std::this_thread::sleep_for(std::chrono::duration<double>(kKillGraceSeconds));
            if (target < 0 || !exit->exited)
                ::kill(target, SIGKILL);
        }).detach();
    }

    int Process::wait()
    {
        if (m_waiter.joinable())
            m_waiter.join();
        while (running())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return m_exitCode;
    }
}
