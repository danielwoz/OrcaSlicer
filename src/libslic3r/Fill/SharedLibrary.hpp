#pragma once

#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// Loading of the shared libraries that carry infill patterns: the built-in pattern libraries (InfillPatternLibraries.cpp)
// and infill plugins (InfillPlugins.cpp).
namespace SharedLibrary {

// HMODULE on Windows, the dlopen() handle elsewhere.
using Handle = void*;

// Loads the library with every symbol resolved now and its symbols private to it. On Windows its own folder and the
// system folders are searched for its dependencies, never the current one, and a broken library or a missing dependency
// is reported instead of shown in a system message box. Returns nullptr and sets error to the system's message when the
// library cannot be loaded.
Handle      open(const boost::filesystem::path &path, std::string &error);
void*       symbol(Handle library, const char *name);
void        close(Handle library);
// Whether the library is loaded in this process, by anyone; it is not loaded by this call.
bool        loaded(const boost::filesystem::path &path);

boost::filesystem::path path_from_utf8(const std::string &utf8);
std::string             path_to_utf8(const boost::filesystem::path &path);

} // namespace SharedLibrary
} // namespace Slic3r
