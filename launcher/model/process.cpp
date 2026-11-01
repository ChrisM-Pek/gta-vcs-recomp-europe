#include "process.h"

#include "system.h"

#include <windows.h>

#include <atomic>

namespace launcher
{
    std::wstring quoteArg(const std::wstring &arg)
    {
        // CommandLineToArgvW's rules: backslashes only escape when they precede a quote
        if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
            return arg;
        std::wstring out = L"\"";
        size_t backslashes = 0;
        for (wchar_t c : arg)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (c == L'"')
                out.append(backslashes * 2 + 1, L'\\');
            else
                out.append(backslashes, L'\\');
            backslashes = 0;
            out += c;
        }
        out.append(backslashes * 2, L'\\');
        return out + L"\"";
    }

    namespace
    {
        std::atomic<void (*)()> g_wakeHook{nullptr};

        void wakeUi()
        {
            if (void (*hook)() = g_wakeHook.load())
                hook();
        }

        void CALLBACK exited(void *, BOOLEAN) { wakeUi(); }

        // The launcher's environment with `extra` set: a CREATE_UNICODE_ENVIRONMENT block.
        std::wstring environmentBlock(const std::vector<std::pair<std::string, std::string>> &extra)
        {
            std::vector<std::wstring> vars;
            wchar_t *env = GetEnvironmentStringsW();
            for (const wchar_t *p = env; *p; p += wcslen(p) + 1)
            {
                std::wstring var(p);
                bool replaced = false;
                for (const auto &[key, value] : extra)
                {
                    const std::wstring k = widen(key) + L"=";
                    if (_wcsnicmp(var.c_str(), k.c_str(), k.size()) == 0)
                        replaced = true;
                }
                if (!replaced)
                    vars.push_back(var);
            }
            FreeEnvironmentStringsW(env);
            for (const auto &[key, value] : extra)
                vars.push_back(widen(key) + L"=" + widen(value));
            std::wstring block;
            for (const auto &v : vars)
                block += v + L'\0';
            return block + L'\0';
        }
    }

    void Process::setWakeHook(void (*hook)()) { g_wakeHook = hook; }

    Process::~Process()
    {
        if (m_exitWait)
            UnregisterWaitEx(m_exitWait, INVALID_HANDLE_VALUE); // and its callback finished
        if (m_reader.joinable())
        {
            // a tool's tree still running, or a leftover grandchild holding the pipe open: the job ends them all
            if (!m_readerDone && m_job)
                TerminateJobObject(m_job, 1);
            m_reader.join();
        }
        if (m_job)
            CloseHandle(m_job); // the game's job doesn't kill on close: it plays on
        if (m_process)
            CloseHandle(m_process);
    }

    bool Process::start(const std::vector<std::wstring> &argv, const Options &options, std::string &error)
    {
        std::wstring cmd;
        for (const auto &a : argv)
            cmd += (cmd.empty() ? L"" : L" ") + quoteArg(a);

        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        HANDLE childOut = nullptr, readEnd = nullptr, nul = nullptr;
        if (options.inheritConsole)
        {
            // ours, explicitly: a console tool started by a GUI exe would otherwise get a console of its own
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        }
        else
        {
            si.dwFlags = STARTF_USESTDHANDLES;
            nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
            si.hStdInput = nul;
            if (!options.outputFile.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(options.outputFile.parent_path(), ec);
                childOut = CreateFileW(options.outputFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (childOut == INVALID_HANDLE_VALUE)
                {
                    error = "cannot write " + narrow(options.outputFile.wstring());
                    CloseHandle(nul);
                    return false;
                }
            }
            else
            {
                if (!CreatePipe(&readEnd, &childOut, &inherit, 1 << 16))
                {
                    error = "CreatePipe failed";
                    CloseHandle(nul);
                    return false;
                }
                SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
            }
            si.hStdOutput = childOut;
            si.hStdError = childOut;
        }

        m_job = CreateJobObjectW(nullptr, nullptr);
        if (m_job && options.killOnClose)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }

        std::wstring env = environmentBlock(options.env);
        DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
        if (!options.inheritConsole || !GetConsoleWindow()) // no console to share: none at all, not a new window
            flags |= options.newConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
        PROCESS_INFORMATION pi{};
        const std::wstring cwd = options.cwd.wstring();
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, env.data(),
                                       cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
        const DWORD lastError = GetLastError();
        if (childOut)
            CloseHandle(childOut);
        if (nul)
            CloseHandle(nul);
        if (!ok)
        {
            if (readEnd)
                CloseHandle(readEnd);
            error = "cannot start " + narrow(argv[0]) + " (" + lastErrorText(lastError) + ")";
            return false;
        }
        if (m_job)
            AssignProcessToJobObject(m_job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        m_process = pi.hProcess;
        m_readPipe = readEnd;
        RegisterWaitForSingleObject(&m_exitWait, m_process, exited, nullptr, INFINITE, WT_EXECUTEONLYONCE);
        if (readEnd)
        {
            m_readerDone = false;
            m_reader = std::thread([this] {
                char buffer[4096];
                DWORD got = 0;
                while (ReadFile(m_readPipe, buffer, sizeof(buffer), &got, nullptr) && got)
                {
                    std::lock_guard<std::mutex> lock(m_lock);
                    for (DWORD i = 0; i < got; ++i)
                    {
                        const char c = buffer[i];
                        if (c == '\n')
                        {
                            if (!m_partial.empty() && m_partial.back() == '\r')
                                m_partial.pop_back();
                            m_lines.push_back(std::move(m_partial));
                            m_partial.clear();
                        }
                        else if (c == '\r' && i + 1 < got && buffer[i + 1] != '\n')
                            m_partial.clear(); // a status line redrawn in place: keep its last version only
                        else
                            m_partial += c;
                    }
                    wakeUi();
                }
                std::lock_guard<std::mutex> lock(m_lock);
                if (!m_partial.empty())
                    m_lines.push_back(std::move(m_partial));
                CloseHandle(m_readPipe);
                m_readPipe = nullptr;
                m_readerDone = true;
                wakeUi();
            });
        }
        return true;
    }

    std::vector<std::string> Process::takeLines()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        std::vector<std::string> out;
        out.swap(m_lines);
        return out;
    }

    bool Process::running()
    {
        if (!m_process)
            return false;
        if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT)
            return true;
        // the rest of its output first - but not forever: a grandchild may keep the pipe open after the tool ended
        const auto now = std::chrono::steady_clock::now();
        if (m_exitSeen == std::chrono::steady_clock::time_point{})
            m_exitSeen = now;
        if (!m_readerDone && now - m_exitSeen < std::chrono::seconds(2))
            return true;
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        m_exitCode = static_cast<int>(code);
        return false;
    }

    int Process::exitCode()
    {
        running();
        return m_exitCode;
    }

    void Process::kill()
    {
        if (m_job)
            TerminateJobObject(m_job, 1);
        else if (m_process)
            TerminateProcess(m_process, 1);
    }

    int Process::wait()
    {
        if (m_process)
            WaitForSingleObject(m_process, INFINITE);
        while (running())
            Sleep(20);
        return m_exitCode;
    }
}
