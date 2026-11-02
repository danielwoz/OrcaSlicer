#include "ModuleLoader.hpp"

#include <algorithm>
#include <set>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>

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

#include <orca_module.h>

#include "../PrintConfig.hpp"
#include "../Fill/FillPlugin.hpp"
#include "ModuleHooks.hpp"
#include "ModuleRegistry.hpp"

namespace Slic3r {
namespace {

namespace fs = boost::filesystem;

// ---- Platform ----

#ifdef _WIN32
using Library = HMODULE;
fs::path    path_from_utf8(const std::string &utf8) { return fs::path(boost::nowide::widen(utf8)); }
std::string path_to_utf8(const fs::path &path) { return boost::nowide::narrow(path.wstring()); }

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

// The library's own directory and the default directories are searched for its dependencies; never the current one.
// A broken library or a missing dependency is reported as an error instead of a system message box.
Library open_library(const fs::path &path, std::string &error)
{
    DWORD previous_mode = 0;
    ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    Library     lib  = ::LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD code = ::GetLastError();
    ::SetThreadErrorMode(previous_mode, nullptr);
    if (lib == nullptr)
        error = "cannot load the library: " + windows_error_message(code);
    return lib;
}
void *find_symbol(Library lib, const char *name) { return reinterpret_cast<void*>(::GetProcAddress(lib, name)); }
void  close_library(Library lib) { ::FreeLibrary(lib); }
// Whether the library is loaded in this process, by anyone; it is not loaded by this call.
bool  library_loaded(const fs::path &path)
{
    HMODULE module = nullptr;
    return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path.wstring().c_str(), &module) != 0;
}
#else
using Library = void*;
fs::path    path_from_utf8(const std::string &utf8) { return fs::path(utf8); }
std::string path_to_utf8(const fs::path &path) { return path.string(); }

// Every symbol is resolved now, so a broken library fails here; its symbols stay private to it.
Library open_library(const fs::path &path, std::string &error)
{
    Library lib = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        const char *message = ::dlerror();
        error = std::string("cannot load the library: ") + (message ? message : "unknown error");
    }
    return lib;
}
void *find_symbol(Library lib, const char *name) { return ::dlsym(lib, name); }
void  close_library(Library lib) { ::dlclose(lib); }
// Whether the library is loaded in this process, by anyone; it is not loaded by this call.
bool  library_loaded(const fs::path &path)
{
    Library lib = ::dlopen(path.string().c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (lib != nullptr)
        ::dlclose(lib);
    return lib != nullptr;
}
#endif

bool is_library_file_name(const fs::path &path)
{
    const std::string ext = path.extension().string();
#if defined(_WIN32)
    return boost::iequals(ext, ".dll");
#elif defined(__APPLE__)
    return boost::iequals(ext, ".dylib") || boost::iequals(ext, ".so");
#else
    return boost::iequals(ext, ".so");
#endif
}

// ---- State of this process; written on the main thread before the registries are sealed ----

bool                    s_disabled = false;
std::set<fs::path>      s_scanned_dirs;
std::set<fs::path>      s_seen_files;
// Libraries whose entry ran; never unloaded, their entry may have started threads or registered atexit handlers.
std::vector<Library>    s_libraries;
// Every load that reported a library or an error, and the ones among them that reported an error.
std::vector<ModuleLoad> s_loads;
std::vector<ModuleLoad> s_failures;

// Registers the module an entry returned into load.
void register_module(const orca_module *returned, ModuleLoad &load)
{
    if (returned == nullptr) {
        load.error = "the module declined OrcaSlicer (module ABI " + std::to_string(ORCA_MODULE_ABI_MAJOR) + "." +
                     std::to_string(ORCA_MODULE_ABI_MINOR) + ")";
        return;
    }
    if (const ModuleRecord *record = ModuleRegistry::register_module(returned, load.error); record != nullptr) {
        load.module   = record->key;
        load.plugin   = record->label + " " + record->version;
        load.patterns = record->patterns;
        for (const ModuleOption *option : record->settings)
            load.options.emplace_back(option->key);
    }
}

// Registers the patterns of the infill plugin an entry returned into load.
void register_plugin(const orca_infill_plugin *returned, ModuleLoad &load)
{
    if (returned == nullptr) {
        load.error = "the plugin declined OrcaSlicer (infill plugin ABI " + std::to_string(ORCA_INFILL_ABI_MAJOR) + "." +
                     std::to_string(ORCA_INFILL_ABI_MINOR) + ")";
        return;
    }
    const orca_infill_plugin plugin = read_abi_struct(returned);
    if (plugin.abi_major != ORCA_INFILL_ABI_MAJOR) {
        load.error = "the plugin was built for infill plugin ABI " + std::to_string(plugin.abi_major) + ", OrcaSlicer supports " +
                     std::to_string(ORCA_INFILL_ABI_MAJOR);
        return;
    }
    if (plugin.struct_size < ORCA_ABI_V1_SIZE(orca_infill_plugin, pattern_count) || plugin.name == nullptr || plugin.version == nullptr ||
        plugin.pattern_count > 64 || (plugin.pattern_count > 0 && plugin.patterns == nullptr)) {
        load.error = "the plugin description is invalid: it needs a name, a version and at most 64 patterns";
        return;
    }
    load.plugin = std::string(plugin.name) + " " + plugin.version;

    std::vector<InfillPluginPattern> accepted;
    for (size_t i = 0; i < plugin.pattern_count; ++ i) {
        InfillPluginPattern pattern;
        std::string         warning;
        if (std::string reason = copy_infill_pattern(plugin.patterns[i], load.plugin, pattern, warning); ! reason.empty())
            append_error(load.error, reason);
        else
            accepted.emplace_back(std::move(pattern));
        if (! warning.empty())
            append_error(load.error, warning);
    }
    for (const InfillPluginPattern *pattern : register_infill_patterns(std::move(accepted), load.error))
        load.patterns.emplace_back(pattern->key);
}

// Calls the entry an opened library exports, a module's or else an infill plugin's, and registers what it returns into
// load. Returns false for a library without either entry, which is closed again.
bool register_library(Library lib, ModuleLoad &load)
{
    const auto module_entry = reinterpret_cast<orca_module_entry_v1_fn>(find_symbol(lib, ORCA_MODULE_ENTRY_V1));
    const auto plugin_entry = module_entry != nullptr ? nullptr :
                              reinterpret_cast<orca_infill_plugin_entry_v1_fn>(find_symbol(lib, ORCA_INFILL_ENTRY_V1));
    if (module_entry == nullptr && plugin_entry == nullptr) {
        // OrcaSlicer took nothing from the library, so it holds no reference into it.
        close_library(lib);
        load.error = "not a module or infill plugin: it exports neither " ORCA_MODULE_ENTRY_V1 " nor " ORCA_INFILL_ENTRY_V1;
        return false;
    }
    s_libraries.push_back(lib);
    const std::string     log_prefix = (module_entry != nullptr ? "Module " : "Infill plugin ") + load.path + ": ";
    const InfillLogSource source(log_prefix);
    const orca_module        *module = nullptr;
    const orca_infill_plugin *plugin = nullptr;
    try {
        if (module_entry != nullptr)
            module = module_entry(&module_host());
        else
            plugin = plugin_entry(&infill_plugin_host());
    } catch (...) {
        load.error = "the entry point of the library threw an exception";
        return true;
    }
    if (module_entry != nullptr)
        register_module(module, load);
    else
        register_plugin(plugin, load);
    return true;
}

// Loads the file load.path into load without reporting the outcome. Returns the canonical path of a library that does
// not export an entry, and an empty path otherwise.
fs::path load_file(ModuleLoad &load)
{
    boost::system::error_code ec;
    const fs::path canonical = fs::canonical(path_from_utf8(load.path), ec);
    if (ec) {
        load.error = "cannot load the library: " + ec.message();
        return {};
    }
    // A file loaded before reports neither a library nor an error.
    if (! s_seen_files.insert(canonical).second)
        return {};
    Library lib = open_library(canonical, load.error);
    if (lib == nullptr)
        return {};
    if (std::find(s_libraries.begin(), s_libraries.end(), lib) != s_libraries.end()) {
        // A library loaded before under another name, such as a hard link or a path spelt differently: the system
        // returned the loaded one with one more reference.
        close_library(lib);
        return {};
    }
    return register_library(lib, load) ? fs::path() : canonical;
}

std::string describe(const ModuleLoad &load)
{
    if (! load.error.empty())
        return (load.module.empty() ? "Module or infill plugin " : "Module ") + load.path + ": " + load.error;
    if (! load.module.empty())
        return "Loaded module " + load.module + " (" + load.plugin + ") from " + load.path + ", settings: " +
               boost::algorithm::join(load.options, ", ") + ", patterns: " + boost::algorithm::join(load.patterns, ", ");
    return "Loaded infill plugin " + load.plugin + " from " + load.path + ", patterns: " + boost::algorithm::join(load.patterns, ", ");
}

// Logs and keeps the outcome of a load; returns false when there is nothing to report.
bool report(const ModuleLoad &load)
{
    if (load.plugin.empty() && load.error.empty())
        return false;
    s_loads.push_back(load);
    if (load.error.empty()) {
        BOOST_LOG_TRIVIAL(info) << describe(load);
    } else {
        BOOST_LOG_TRIVIAL(error) << describe(load);
        s_failures.push_back(load);
    }
    return true;
}

// Whether a command line flag value turns a switch on, as read_cli() reads it.
bool switch_on(bool has_value, const std::string &value)
{
    return ! has_value || value == "1" || boost::iequals(value, "true") || boost::iequals(value, "yes") || boost::iequals(value, "on") ||
           boost::iequals(value, "enabled");
}

void append_path_list(std::vector<std::string> &dirs, const char *variable)
{
    const char *env = boost::nowide::getenv(variable);
    if (env == nullptr)
        return;
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const std::string paths = env;
    for (size_t begin = 0; begin <= paths.size();) {
        size_t end = paths.find(separator, begin);
        if (end == std::string::npos)
            end = paths.size();
        if (end > begin)
            dirs.emplace_back(paths.substr(begin, end - begin));
        begin = end + 1;
    }
}

// The names of the directories searched for modules in each location; infill_plugins also holds infill pattern plugins.
constexpr const char *dir_names[] = { "modules", "infill_plugins" };

} // namespace

// ---- Loading ----

ModuleStartup module_startup(int argc, char **argv, const boost::filesystem::path &program)
{
    ModuleStartup out;
    std::string   data_dir;
    // The values of the options read here, as read_cli() reads them: "--key value" or "--key=value", up to "--".
    for (int i = 1; i < argc; ++ i) {
        std::string token = argv[i];
        if (token == "--")
            break;
        if (! boost::starts_with(token, "-"))
            continue;
        token.erase(0, boost::starts_with(token, "--") ? 2 : 1);
        std::string value;
        bool        has_value = false;
        if (const size_t equals = token.find('='); equals != std::string::npos) {
            value     = token.substr(equals + 1);
            has_value = true;
            token.erase(equals);
        }
        if (token == "no-modules" || token == "no-infill-plugins") {
            out.disabled = switch_on(has_value, value);
        } else if (token == "module-dir" || token == "infill-plugin-dir" || token == "datadir") {
            if (! has_value) {
                if (i + 1 >= argc)
                    break;
                value = argv[++ i];
            }
            if (token == "datadir") {
                data_dir = value;
            } else {
                ConfigOptionStrings dirs;
                if (dirs.deserialize(value))
                    append(out.dirs, dirs.values);
            }
        }
    }

    for (const char *variable : { "ORCA_MODULES", "ORCA_INFILL_PLUGINS" })
        if (const char *env = boost::nowide::getenv(variable); env != nullptr && std::string(env) == "0")
            out.disabled = true;
    append_path_list(out.dirs, "ORCA_MODULE_PATH");
    append_path_list(out.dirs, "ORCA_INFILL_PLUGIN_PATH");

    const fs::path exe_dir = program.parent_path();
    for (const char *name : dir_names) {
#if defined(_WIN32)
        out.dirs.emplace_back(path_to_utf8(exe_dir / name));
#elif defined(__APPLE__)
        // OrcaSlicer.app/Contents/MacOS/<exe> -> OrcaSlicer.app/Contents/PlugIns/<name>
        out.dirs.emplace_back(path_to_utf8(exe_dir.parent_path() / "PlugIns" / name));
#else
        // <prefix>/bin/<exe> -> <prefix>/lib/orca-slicer/<name>, also inside an AppImage.
        out.dirs.emplace_back(path_to_utf8(exe_dir.parent_path() / "lib" / "orca-slicer" / name));
#endif
    }
    if (! data_dir.empty())
        append(out.dirs, module_data_dirs(data_dir));
    return out;
}

std::vector<std::string> module_data_dirs(const std::string &data_dir)
{
    std::vector<std::string> out;
    for (const char *name : dir_names)
        out.emplace_back(path_to_utf8(path_from_utf8(data_dir) / name));
    return out;
}

std::vector<ModuleLoad> load_modules(const std::vector<std::string> &dirs)
{
    std::vector<ModuleLoad> out;
    if (s_disabled || ModuleRegistry::sealed())
        return out;
    for (const std::string &dir : dirs) {
        boost::system::error_code ec;
        const fs::path canonical = fs::canonical(path_from_utf8(dir), ec);
        if (ec || ! fs::is_directory(canonical, ec)) {
            BOOST_LOG_TRIVIAL(debug) << "Module directory " << dir << " does not exist";
            continue;
        }
        if (! s_scanned_dirs.insert(canonical).second)
            continue;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(canonical, ec), end; ! ec && it != end; it.increment(ec))
            if (is_library_file_name(it->path()) && fs::is_regular_file(it->path(), ec))
                files.emplace_back(it->path());
        if (ec)
            BOOST_LOG_TRIVIAL(error) << "Module directory " << dir << " cannot be read: " << ec.message();
        std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) { return a.filename() < b.filename(); });
        // A library without an entry can be a dependency of a module next to it. It is reported only when nothing keeps
        // it loaded once the whole directory is loaded.
        std::vector<std::pair<ModuleLoad, fs::path>> without_entry;
        for (const fs::path &file : files) {
            ModuleLoad load;
            load.path = path_to_utf8(file);
            if (fs::path library = load_file(load); ! library.empty())
                without_entry.emplace_back(std::move(load), std::move(library));
            else if (report(load))
                out.emplace_back(std::move(load));
        }
        for (auto &[load, library] : without_entry)
            if (library_loaded(library))
                BOOST_LOG_TRIVIAL(debug) << "Module directory " << dir << ": " << load.path << " is a library that another one loaded";
            else if (report(load))
                out.emplace_back(std::move(load));
    }
    return out;
}

ModuleLoad load_module(const std::string &path)
{
    ModuleLoad load;
    load.path = path;
    if (s_disabled || ModuleRegistry::sealed()) {
        load.error = "modules are disabled";
        return load;
    }
    load_file(load);
    report(load);
    return load;
}

void disable_modules() { s_disabled = true; }

const std::vector<ModuleLoad>& module_load_failures() { return s_failures; }

void log_module_loads()
{
    for (const ModuleLoad &load : s_loads)
        BOOST_LOG_TRIVIAL(info) << describe(load);
}

} // namespace Slic3r
