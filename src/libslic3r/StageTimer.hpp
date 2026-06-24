#pragma once
// TEMPORARY profiling helper (perf-campaign): env-gated wall-clock stage timing.
// Enable with ORCA_STAGE_TIMING=1. Zero overhead when the env var is unset
// (the getenv is read once). Prints "[STAGE] <name>: <ms> ms" to stderr.
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace Slic3r {

inline bool stage_timing_enabled() {
    static const bool en = [] {
        const char* v = std::getenv("ORCA_STAGE_TIMING");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return en;
}

class StageTimer {
public:
    explicit StageTimer(std::string name) : m_name(std::move(name)) {
        if (stage_timing_enabled())
            m_t0 = std::chrono::steady_clock::now();
    }
    ~StageTimer() {
        if (stage_timing_enabled()) {
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - m_t0).count();
            std::fprintf(stderr, "[STAGE] %-32s %9.1f ms\n", m_name.c_str(), ms);
            std::fflush(stderr);
        }
    }
private:
    std::string m_name;
    std::chrono::steady_clock::time_point m_t0;
};

} // namespace Slic3r
