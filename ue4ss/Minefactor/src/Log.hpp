#pragma once
#include <format>
#include <string>

namespace mf
{
    // One line to UE4SS.log, prefixed [Minefactor].
    void LogLine(const std::wstring& line);

    template <class... Args>
    void Log(std::wformat_string<Args...> fmt, Args&&... args)
    {
        LogLine(std::format(fmt, std::forward<Args>(args)...));
    }
}
