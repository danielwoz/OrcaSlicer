#include <catch2/catch_test_macros.hpp>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/InfillPatternRegistry.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include "corpus.hpp"

using namespace Slic3r;

namespace {

// The platforms the golden digests are kept for: the output of a filler may differ in the last bits between
// compilers, standard libraries and instruction sets.
const char* golden_platform()
{
#if defined(_WIN32) && defined(_M_ARM64)
    return "win-arm64";
#elif defined(_WIN32)
    return "win-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#else
    return "unknown";
#endif
}

// One line of the golden file: case, digest, paths, points.
std::string golden_line(const InfillCorpus::Case &c, const InfillCorpus::Digest &d)
{
    char digest[17];
    std::snprintf(digest, sizeof(digest), "%016" PRIx64, d.hash);
    std::ostringstream out;
    out << c.name << '\t' << digest << '\t' << d.paths << '\t' << d.points;
    return out.str();
}

} // namespace

TEST_CASE("Native fillers reproduce the golden corpus", "[InfillPatterns][Golden]")
{
    // Every case runs on a fresh native filler under its own scaling factor.
    InfillCorpus::NativeStates states;
    std::vector<std::string>   lines;
    const double               saved = SCALING_FACTOR;
    for (const InfillCorpus::Case &c : InfillCorpus::cases()) {
        // A row this build leaves out has no digest to check.
        if (! InfillPatternRegistry::builtins()[size_t(c.pattern)].available())
            continue;
        set_scaling_factor(c.scaling_factor);
        std::unique_ptr<Fill> filler(Fill::new_from_type(c.pattern));
        InfillCorpus::apply_members(c, *filler);
        states.attach(c, *filler);
        lines.emplace_back(golden_line(c, InfillCorpus::digest(c, InfillCorpus::run(c, *filler))));
    }
    set_scaling_factor(saved);

    // ORCA_INFILL_GOLDEN_WRITE=<path> writes the digests of this platform instead of checking them.
    if (const char *path = std::getenv("ORCA_INFILL_GOLDEN_WRITE"); path != nullptr && *path != 0) {
        std::ofstream file(path, std::ios::binary);
        file << "# Digests of the native filler output of tests/infill_patterns/corpus.cpp on " << golden_platform() << ".\n"
             << "# case\tdigest\tpaths\tpoints\n";
        for (const std::string &line : lines)
            file << line << '\n';
        REQUIRE(file.good());
        return;
    }

    const boost::filesystem::path golden = boost::filesystem::path(TEST_DATA_DIR) / "infill_golden" / (std::string(golden_platform()) + ".tsv");
    if (! boost::filesystem::exists(golden))
        SKIP("No golden digests for " << golden_platform());

    std::map<std::string, std::string> expected;
    {
        std::ifstream file(golden.string(), std::ios::binary);
        for (std::string line; std::getline(file, line);) {
            // A checkout may have turned the line ends into CR LF.
            if (! line.empty() && line.back() == '\r')
                line.pop_back();
            if (! line.empty() && line.front() != '#')
                expected.emplace(line.substr(0, line.find('\t')), line);
        }
    }
    std::vector<std::string> differing, missing;
    for (const std::string &line : lines) {
        const std::string name = line.substr(0, line.find('\t'));
        auto              it   = expected.find(name);
        if (it == expected.end())
            missing.emplace_back(name);
        else if (it->second != line)
            differing.emplace_back(line + "  (golden: " + it->second.substr(name.size() + 1) + ")");
    }
    std::string report;
    for (const std::string &line : differing)
        report += line + '\n';
    INFO("Differing cases:\n" << report);
    CHECK(differing.empty());
    INFO("Cases without a golden digest: " << missing.size());
    CHECK(missing.empty());
}
