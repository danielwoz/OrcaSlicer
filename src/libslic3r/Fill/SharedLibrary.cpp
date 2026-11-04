#include "SharedLibrary.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/nowide/convert.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Slic3r {
namespace SharedLibrary {

#ifdef _WIN32
namespace {

std::string windows_error_message(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                          nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string message = length > 0 ? boost::nowide::narrow(buffer, size_t(length)) : "error " + std::to_string(code);
    ::LocalFree(buffer);
    boost::algorithm::trim(message);
    return message;
}

} // namespace

Handle open(const boost::filesystem::path &path, std::string &error)
{
    DWORD previous_mode = 0;
    ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    HMODULE     lib  = ::LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD code = ::GetLastError();
    ::SetThreadErrorMode(previous_mode, nullptr);
    if (lib == nullptr)
        error = windows_error_message(code);
    return reinterpret_cast<Handle>(lib);
}

void* symbol(Handle library, const char *name)
{
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(library), name));
}

void close(Handle library) { ::FreeLibrary(reinterpret_cast<HMODULE>(library)); }

bool loaded(const boost::filesystem::path &path)
{
    HMODULE module = nullptr;
    return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path.wstring().c_str(), &module) != 0;
}

boost::filesystem::path path_from_utf8(const std::string &utf8) { return boost::filesystem::path(boost::nowide::widen(utf8)); }
std::string             path_to_utf8(const boost::filesystem::path &path) { return boost::nowide::narrow(path.wstring()); }
#else
Handle open(const boost::filesystem::path &path, std::string &error)
{
    Handle lib = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        const char *message = ::dlerror();
        error = message ? message : "unknown error";
    }
    return lib;
}

void* symbol(Handle library, const char *name) { return ::dlsym(library, name); }

void close(Handle library) { ::dlclose(library); }

bool loaded(const boost::filesystem::path &path)
{
    Handle lib = ::dlopen(path.string().c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (lib != nullptr)
        ::dlclose(lib);
    return lib != nullptr;
}

boost::filesystem::path path_from_utf8(const std::string &utf8) { return boost::filesystem::path(utf8); }
std::string             path_to_utf8(const boost::filesystem::path &path) { return path.string(); }
#endif

} // namespace SharedLibrary
} // namespace Slic3r
