// A child process for the launcher: the tools (python tools/*.py, their output read line by line) and the game
// (its output to a log file). Cancel or closing the launcher stops the whole tree (build.py -> cmake -> ninja ->
// clang): on Windows each runs in a Job Object, on Linux in a process group of its own (process_win32.cpp,
// process_posix.cpp; the line handling is shared, process.cpp).
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
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

        // argv[0] is the program (UTF-8; found on PATH when it has no folder).
        bool start(const std::vector<std::string> &argv, const Options &options, std::string &error);

        // Lines printed since the last call (UTF-8, without line ends).
        std::vector<std::string> takeLines();
        bool running();
        // valid once running() is false. Linux: a death by signal N is 128 + N, as a shell reports it
        int exitCode();
        void kill();
        int wait();     // blocks; the exit code

        // called from other threads when a process prints or ends, so the UI wakes instead of polling
        static void setWakeHook(void (*hook)());
        static void wakeUi();

    private:
        void consume(const char *data, size_t size); // the reader thread: bytes into lines
        void finishReading();                        // the reader thread: the pipe closed

#ifdef _WIN32
        void *m_process = nullptr;
        void *m_job = nullptr;
        void *m_readPipe = nullptr;
        void *m_exitWait = nullptr; // a thread-pool wait on the process: wakes the UI when it ends
#else
        struct Exit;                // what the waiter thread saw (shared: the game's waiter may outlive us)
        int m_pid = -1;             // also the process group (killOnClose) or session (the game) it leads
        int m_readFd = -1;
        int m_stopFd = -1;          // an eventfd: tells the reader to stop (a grandchild may hold the pipe open)
        bool m_ownGroup = false;    // kill() signals the whole group
        bool m_killOnClose = true;
        std::thread m_waiter;       // blocks in waitpid, wakes the UI at the exit
        std::shared_ptr<Exit> m_exit;
#endif
        std::thread m_reader;
        std::mutex m_lock;
        std::vector<std::string> m_lines;
        std::string m_partial;
        std::atomic<bool> m_readerDone{true};
        std::chrono::steady_clock::time_point m_exitSeen{};
        int m_exitCode = -1;
    };
}
