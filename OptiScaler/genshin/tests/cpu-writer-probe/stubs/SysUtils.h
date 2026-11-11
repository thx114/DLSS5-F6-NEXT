// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <cstring>
#include <stdexcept>

namespace ProbeTestLog {
inline unsigned summaries = 0, payloads = 0, bindings = 0, expectations = 0;
inline bool fail = false;
template<class... Args> void Info(const char* format, const Args&...) {
    if (fail) throw std::runtime_error("test log failure");
    if (std::strstr(format, "probe: pid=")) ++summaries;
    else if (std::strstr(format, "CB binding:")) ++bindings;
    else if (std::strstr(format, "CB: pid=")) ++payloads;
    else if (std::strstr(format, "expectation:")) ++expectations;
}
}
#define LOG_INFO(...) ProbeTestLog::Info(__VA_ARGS__)
