// Windows pieces the launcher needs: text conversion, folders, Explorer, shortcuts, keeping the PC awake.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace launcher
{
    std::wstring widen(const std::string &utf8);
    std::string narrow(const std::wstring &wide);
    std::string lastErrorText(unsigned long code);
    inline std::string pathText(const std::filesystem::path &p) { return narrow(p.wstring()); }
    inline std::filesystem::path pathFrom(const std::string &utf8) { return std::filesystem::path(widen(utf8)); }

    std::filesystem::path exePath();
    enum class Folder
    {
        Desktop,
        Downloads,
        Documents,
        LocalAppData
    };
    std::filesystem::path knownFolder(Folder which);

    void openFolder(const std::filesystem::path &dir);   // Explorer at the folder
    void revealFile(const std::filesystem::path &file);  // Explorer with the file selected
    void openUrl(const std::string &url);
    void openFile(const std::filesystem::path &file);     // its default program (a log in Notepad)
    bool createShortcut(const std::filesystem::path &link, const std::filesystem::path &target,
                        const std::filesystem::path &workingDir, const std::string &description, std::string &error);
    void keepAwake(bool on);                              // no sleep while a build runs
    bool attachParentConsole();                           // CLI mode: print into the terminal that started us
    bool redirectOutput(const std::filesystem::path &file); // CLI mode --out: our output and the tools' to a file
    uint64_t freeBytes(const std::filesystem::path &dir);
}
