// What every platform's Process shares: the wake hook and turning the child's bytes into lines.
#include "process.h"

namespace launcher
{
    namespace
    {
        std::atomic<void (*)()> g_wakeHook{nullptr};
    }

    void Process::setWakeHook(void (*hook)()) { g_wakeHook = hook; }

    void Process::wakeUi()
    {
        if (void (*hook)() = g_wakeHook.load())
            hook();
    }

    void Process::consume(const char *data, size_t size)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            for (size_t i = 0; i < size; ++i)
            {
                const char c = data[i];
                if (c == '\n')
                {
                    if (!m_partial.empty() && m_partial.back() == '\r')
                        m_partial.pop_back();
                    m_lines.push_back(std::move(m_partial));
                    m_partial.clear();
                }
                else if (c == '\r' && i + 1 < size && data[i + 1] != '\n')
                    m_partial.clear(); // a status line redrawn in place: keep its last version only
                else
                    m_partial += c;
            }
        }
        wakeUi();
    }

    void Process::finishReading()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!m_partial.empty())
                m_lines.push_back(std::move(m_partial));
            m_partial.clear();
            m_readerDone = true;
        }
        wakeUi();
    }

    std::vector<std::string> Process::takeLines()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        std::vector<std::string> out;
        out.swap(m_lines);
        return out;
    }
}
