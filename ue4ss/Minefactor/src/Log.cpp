#include "Log.hpp"

#include <DynamicOutput/DynamicOutput.hpp>

namespace mf
{
    void LogLine(const std::wstring& line)
    {
        // The text goes in as a {} argument so braces inside it aren't format specifiers.
        RC::Output::send<RC::LogLevel::Default>(STR("[Minefactor] {}\n"), line);
    }
}
