#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "../Polyline.hpp"
#include "../PrintConfig.hpp"

struct orca_infill_builtin_host;
struct orca_infill_builtin_library;
struct orca_infill_sink;

// The context of one prepare_state() call of a pattern library (orca_infill_builtin.h).
struct orca_infill_job
{
    // Throws CanceledException when the work is to stop; may be empty.
    std::function<void()> throw_if_canceled;
};

namespace Slic3r {

// The built-in infill patterns served by pattern libraries of their own (src/infill_patterns): one library per row of
// the pattern table, loaded from the application's folder, checked and bound to its row (InfillPatternRegistry::bound()).
// A library of another build, or one that describes its row differently, is never bound.

// ORCA_INFILL_BUILTIN_ENTRY_V1.
using BuiltinInfillEntry = const orca_infill_builtin_library *(*)(const orca_infill_builtin_host *host);

// The outcome of loading the library of one row.
struct BuiltinInfillLoad
{
    InfillPattern                      pattern { ipCount };
    // orca_infill_<token>.
    std::string                        name;
    // The file name, or name for a library linked into the executable.
    std::string                        file;
    // The file opened; empty for a linked library.
    boost::filesystem::path            path;
    // The entry, when the library has one.
    BuiltinInfillEntry                 entry { nullptr };
    // The library once it passed every check.
    const orca_infill_builtin_library *library { nullptr };
    // Why the library is not bound; empty when it is.
    std::string                        error;
    // Time taken to open and check the library.
    double                             milliseconds { 0. };
};

// A pattern library linked into an executable (src/infill_patterns/runtime/LinkedLibraries.cpp).
struct LinkedBuiltinInfillLibrary
{
    InfillPattern      pattern;
    const char        *name;
    BuiltinInfillEntry entry;
};

// The folders the libraries are looked for in, in order, around the executable program: infill_patterns on Windows,
// ../PlugIns/infill_patterns on macOS, and ../<libdir>/orca-slicer/infill_patterns, ../lib/orca-slicer/infill_patterns and
// infill_patterns on Linux.
std::vector<boost::filesystem::path> builtin_infill_pattern_folders(const boost::filesystem::path &program);

// Loads the library of every row a pattern library serves from the first of folders that exists, under the file name
// of the row, and binds those that pass every check; folders are never mixed and no other file is opened. With
// ORCA_INFILL_PATTERN_LINKAGE=STATIC binds the libraries linked into the executable instead. Main thread, before
// InfillPatternRegistry::seal(). Runs once per process: a later call returns the outcome of the first, in InfillPattern
// order.
const std::vector<BuiltinInfillLoad>& load_builtin_infill_patterns(const std::vector<boost::filesystem::path> &folders);
// Binds libraries linked into the executable, like load_builtin_infill_patterns(), and also only once per process.
const std::vector<BuiltinInfillLoad>& bind_builtin_infill_patterns(const std::vector<LinkedBuiltinInfillLibrary> &libraries);
// The libraries of orca_infill_patterns_linked, defined in the executables that link it (src/infill_patterns/CMakeLists.txt).
const std::vector<LinkedBuiltinInfillLibrary>& linked_builtin_infill_patterns();
// The outcome of loading so far; empty before.
const std::vector<BuiltinInfillLoad>& builtin_infill_pattern_loads();
// The folder the libraries were loaded from; empty when none of the folders exists or the libraries are linked.
const boost::filesystem::path& builtin_infill_pattern_folder();
// Writes the outcome of loading to the log, one line per library, for the loads made before the log was set up.
void log_builtin_infill_pattern_loads();
// Prints the value, token, file, status and load time of every row a pattern library serves (--check-infill-patterns).
void print_builtin_infill_patterns(std::ostream &out);

// Opens the file and runs every check on it as the library of row, without binding it. A library whose entry ran stays
// loaded, as the loader keeps every library it called into.
BuiltinInfillLoad open_builtin_infill_library(const boost::filesystem::path &path, InfillPattern row);
// Calls entry and runs the checks of the library it returns as the library of row, without binding it.
BuiltinInfillLoad check_builtin_infill_entry(BuiltinInfillEntry entry, InfillPattern row);

// "Infill pattern library <file> is missing or damaged (<reason>). Reinstall OrcaSlicer." for a row whose library is
// not bound.
std::string builtin_infill_library_error(InfillPattern row);

// Ends every call into the libraries: run at exit. A state or a filler released afterwards is left to the process
// instead of being released into a library whose static objects may be gone.
void shutdown_builtin_infill_patterns();
bool builtin_infill_patterns_shut_down();

// The host table of the application (orca_infill_builtin.h).
const orca_infill_builtin_host& builtin_infill_host();

// The output of one run() or query_state() of a library, as it emits it, behind its orca_infill_sink.
struct BuiltinInfillOutput
{
    Polylines                                 polylines;
    ThickPolylines                            thick_polylines;
    // The polylines that follow each begin_group(), with its flow index.
    std::vector<std::pair<size_t, Polylines>> groups;
    // Fill::spacing after the call.
    std::optional<double>                     spacing;
    std::string                               error;

    orca_infill_sink* sink() { return reinterpret_cast<orca_infill_sink*>(this); }
};

} // namespace Slic3r
