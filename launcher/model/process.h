// A child process for the launcher: the tools (python tools/*.py, their output read line by line) and the game
// (its output to a log file). Each runs in a Job Object, so Cancel or closing the launcher stops the whole tree
// (build.py -> cmake -> ninja -> clang-cl).
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace launcher
{
    class Process
    {
    public:
        Process() = default;
        ~Process();
        Process(const Process &) = delete;
        Process &operator=(const Process &) = delete;

        struct Options
        {
            std::filesystem::path cwd;
            std::vector<std::pair<std::string, std::string>> env; // added to / replacing the launcher's own
            std::filesystem::path outputFile;                     // stdout + stderr to this file instead of lines()
            bool killOnClose = true;                              // false: the game outlives the launcher
            bool newConsole = false;                              // CLI mode passes its console through instead
            bool inheritConsole = false;
        };

        // argv[0] is the program; quoted for CreateProcess here.
        bool start(const std::vector<std::wstring> &argv, const Options &options, std::string &error);

        // Lines printed since the last call (UTF-8, without line ends).
        std::vector<std::string> takeLines();
        bool running();
        int exitCode(); // valid once running() is false
        void kill();
        int wait();     // blocks; the exit code

        // called from other threads when a process prints or ends, so the UI wakes instead of polling
        static void setWakeHook(void (*hook)());

    private:
        void *m_process = nullptr;
        void *m_job = nullptr;
        void *m_readPipe = nullptr;
        void *m_exitWait = nullptr; // a thread-pool wait on the process: wakes the UI when it ends
        std::thread m_reader;
        std::mutex m_lock;
        std::vector<std::string> m_lines;
        std::string m_partial;
        std::atomic<bool> m_readerDone{true};
        std::chrono::steady_clock::time_point m_exitSeen{};
        int m_exitCode = -1;
    };

    std::wstring quoteArg(const std::wstring &arg);
}
