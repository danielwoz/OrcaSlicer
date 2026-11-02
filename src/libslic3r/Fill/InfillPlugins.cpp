#include "InfillPlugins.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
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

#include "libslic3r_version.h"
#include "../ClipperUtils.hpp"
#include "../I18N.hpp"
#include "../LocalesUtils.hpp"
#include "../format.hpp"
#include "FillPlugin.hpp"
#include "InfillPatternRegistry.hpp"

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

// The plugin's own directory and the default directories are searched for its dependencies; never the current one.
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

// Every symbol is resolved now, so a broken plugin fails here; its symbols stay private to it.
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

bool is_plugin_file_name(const fs::path &path)
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

// ---- State of this process; written on the main thread before seal() only ----

bool                              s_disabled = false;
std::set<fs::path>                s_scanned_dirs;
std::set<fs::path>                s_seen_files;
// Libraries whose entry ran; never unloaded, their entry may have started threads or registered atexit handlers.
std::vector<Library>              s_libraries;
// Addresses stay valid while patterns are appended.
std::deque<InfillPluginPattern>   s_patterns;
// Every load that reported a plugin or an error, and the ones among them that reported an error.
std::vector<InfillPluginLoad>     s_loads;
std::vector<InfillPluginLoad>     s_failures;

// The pattern whose generate() runs on this thread, and the file whose entry runs; they name the source of log lines.
thread_local const InfillPluginPattern *t_generating = nullptr;
thread_local const std::string         *t_loading    = nullptr;

// ---- Host services: callable from any thread, never throw ----

Points to_points(const orca_infill_point *points, size_t count)
{
    Points out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++ i)
        out.emplace_back(coord_t(points[i].x), coord_t(points[i].y));
    return out;
}

ExPolygon to_expolygon(const orca_infill_region &region)
{
    ExPolygon out;
    out.contour.points = to_points(region.contour.points, region.contour.count);
    for (size_t i = 0; i < region.hole_count; ++ i)
        out.holes.emplace_back(to_points(region.holes[i].points, region.holes[i].count));
    return out;
}

bool valid_path(const orca_infill_path &path) { return path.count == 0 || path.points != nullptr; }

bool valid_region(const orca_infill_region *region)
{
    if (region == nullptr || ! valid_path(region->contour) || (region->hole_count > 0 && region->holes == nullptr))
        return false;
    for (size_t i = 0; i < region->hole_count; ++ i)
        if (! valid_path(region->holes[i]))
            return false;
    return true;
}

orca_infill_status ORCA_INFILL_CALL host_emit_polyline(orca_infill_sink *sink, const orca_infill_point *points, size_t count) noexcept
{
    if (sink == nullptr || points == nullptr || count < 2)
        return ORCA_INFILL_ERR_INVALID;
    try {
        reinterpret_cast<Polylines*>(sink)->emplace_back(to_points(points, count));
        return ORCA_INFILL_OK;
    } catch (const std::bad_alloc &) {
        return ORCA_INFILL_ERR_NOMEM;
    } catch (...) {
        return ORCA_INFILL_ERR_FAILED;
    }
}

void ORCA_INFILL_CALL host_log(int32_t level, const char *message) noexcept
{
    try {
        const std::string source = t_generating != nullptr ? t_generating->plugin + " " + t_generating->key :
                                   t_loading != nullptr    ? *t_loading : std::string();
        const std::string text   = "Infill plugin " + source + ": " + (message != nullptr ? message : "");
        switch (level) {
        case ORCA_INFILL_LOG_DEBUG:   BOOST_LOG_TRIVIAL(debug) << text; break;
        case ORCA_INFILL_LOG_INFO:    BOOST_LOG_TRIVIAL(info) << text; break;
        case ORCA_INFILL_LOG_WARNING: BOOST_LOG_TRIVIAL(warning) << text; break;
        default:                      BOOST_LOG_TRIVIAL(error) << text; break;
        }
    } catch (...) {
    }
}

orca_infill_status ORCA_INFILL_CALL host_clip_polylines(const orca_infill_path *polylines, size_t count, const orca_infill_region *region,
                                                        orca_infill_path_visitor visit, void *user) noexcept
{
    if ((polylines == nullptr && count > 0) || ! valid_region(region) || visit == nullptr)
        return ORCA_INFILL_ERR_INVALID;
    try {
        Polylines lines;
        lines.reserve(count);
        for (size_t i = 0; i < count; ++ i)
            if (polylines[i].count >= 2 && polylines[i].points != nullptr)
                lines.emplace_back(to_points(polylines[i].points, polylines[i].count));
        std::vector<orca_infill_point> piece;
        for (const Polyline &line : intersection_pl(lines, to_expolygon(*region))) {
            piece.clear();
            for (const Point &p : line.points)
                piece.push_back({ p.x(), p.y() });
            visit(user, piece.data(), piece.size());
        }
        return ORCA_INFILL_OK;
    } catch (const std::bad_alloc &) {
        return ORCA_INFILL_ERR_NOMEM;
    } catch (...) {
        return ORCA_INFILL_ERR_FAILED;
    }
}

orca_infill_status ORCA_INFILL_CALL host_offset_region(const orca_infill_region *region, double delta_mm,
                                                       orca_infill_region_visitor visit, void *user) noexcept
{
    if (! valid_region(region) || ! std::isfinite(delta_mm) || visit == nullptr)
        return ORCA_INFILL_ERR_INVALID;
    try {
        for (const ExPolygon &expolygon : offset_ex(to_expolygon(*region), float(scale_(delta_mm)))) {
            const InfillPluginRegion out(expolygon);
            visit(user, &out.view());
        }
        return ORCA_INFILL_OK;
    } catch (const std::bad_alloc &) {
        return ORCA_INFILL_ERR_NOMEM;
    } catch (...) {
        return ORCA_INFILL_ERR_FAILED;
    }
}

const orca_infill_host s_host { sizeof(orca_infill_host), ORCA_INFILL_ABI_MAJOR, ORCA_INFILL_ABI_MINOR, SoftFever_VERSION,
                                &host_emit_polyline, &host_log, &host_clip_polylines, &host_offset_region };

// ---- Validation ----

// Size of the 1.0 layout of a struct: up to the end of its last 1.0 field.
#define ORCA_INFILL_V1_SIZE(TYPE, LAST) (offsetof(TYPE, LAST) + sizeof(TYPE::LAST))

// A plugin struct read into the host's layout: fields beyond the plugin's struct_size are zero, fields the host does
// not know are ignored.
template<class T> T read_struct(const T *source)
{
    T out {};
    uint32_t size = 0;
    std::memcpy(&size, source, sizeof(size));
    std::memcpy(&out, source, std::min<size_t>(size, sizeof(T)));
    out.struct_size = size;
    return out;
}

bool matches_charset(const char *s, size_t max_length, bool allow_dash)
{
    if (s == nullptr)
        return false;
    const size_t length = std::strlen(s);
    if (length == 0 || length > max_length)
        return false;
    return std::all_of(s, s + length, [allow_dash](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (allow_dash && c == '-');
    });
}

bool integral(double v) { return std::isfinite(v) && v == std::floor(v); }
bool int32_value(double v)
{ return integral(v) && v >= double(std::numeric_limits<int32_t>::min()) && v <= double(std::numeric_limits<int32_t>::max()); }

// The reason a parameter is rejected; empty when it is valid.
std::string check_param(const orca_infill_param &p, const std::vector<InfillPluginParam> &previous)
{
    if (p.struct_size < ORCA_INFILL_V1_SIZE(orca_infill_param, enum_count))
        return "a parameter has an unknown layout";
    if (! matches_charset(p.name, 32, false))
        return "a parameter name must match [a-z0-9_]{1,32}";
    const std::string name = p.name;
    if (std::any_of(previous.begin(), previous.end(), [&name](const InfillPluginParam &other) { return other.name == name; }))
        return "parameter " + name + " is declared twice";
    if (p.label == nullptr)
        return "parameter " + name + " has no label";
    switch (p.type) {
    case ORCA_INFILL_PARAM_FLOAT:
    case ORCA_INFILL_PARAM_INT:
        if (! std::isfinite(p.min_value) || ! std::isfinite(p.max_value) || ! std::isfinite(p.default_value) ||
            p.min_value > p.default_value || p.default_value > p.max_value)
            return "parameter " + name + " needs finite values with min <= default <= max";
        if (p.type == ORCA_INFILL_PARAM_INT && ! (int32_value(p.min_value) && int32_value(p.max_value) && int32_value(p.default_value)))
            return "parameter " + name + " needs integral values within the range of a 32-bit integer";
        break;
    case ORCA_INFILL_PARAM_BOOL:
        if (p.default_value != 0. && p.default_value != 1.)
            return "parameter " + name + " needs a default of 0 or 1";
        break;
    case ORCA_INFILL_PARAM_ENUM: {
        if (p.enum_count < 1 || p.enum_count > 64 || p.enum_values == nullptr || p.enum_labels == nullptr)
            return "parameter " + name + " needs 1 to 64 values";
        std::set<std::string> values;
        for (size_t i = 0; i < p.enum_count; ++ i) {
            if (! matches_charset(p.enum_values[i], 32, true) || ! values.insert(p.enum_values[i]).second)
                return "parameter " + name + " needs distinct values matching [a-z0-9_-]{1,32}";
            if (p.enum_labels[i] == nullptr)
                return "parameter " + name + " has a value without a label";
        }
        if (! integral(p.default_value) || p.default_value < 0. || p.default_value >= double(p.enum_count))
            return "parameter " + name + " needs a default index below the number of values";
        break;
    }
    default:
        return "parameter " + name + " has an unknown type";
    }
    return {};
}

InfillPluginParam copy_param(const orca_infill_param &p)
{
    InfillPluginParam out;
    out.name          = p.name;
    out.label         = p.label;
    out.tooltip       = p.tooltip != nullptr ? p.tooltip : "";
    out.unit          = p.unit != nullptr ? p.unit : "";
    out.type          = p.type;
    out.default_value = p.default_value;
    if (p.type == ORCA_INFILL_PARAM_ENUM) {
        out.max_value = double(p.enum_count - 1);
        for (size_t i = 0; i < p.enum_count; ++ i) {
            out.enum_values.emplace_back(p.enum_values[i]);
            out.enum_labels.emplace_back(p.enum_labels[i]);
        }
    } else if (p.type == ORCA_INFILL_PARAM_BOOL) {
        out.max_value = 1.;
    } else {
        out.min_value = p.min_value;
        out.max_value = p.max_value;
    }
    return out;
}

constexpr uint32_t known_caps = ORCA_INFILL_CAP_MULTILINE | ORCA_INFILL_CAP_SEPARABLE | ORCA_INFILL_CAP_FIXED_ANGLE |
                                ORCA_INFILL_CAP_KEEP_ORDER | ORCA_INFILL_CAP_ROTATE_TEMPLATE_SAFE;
constexpr uint32_t known_post = ORCA_INFILL_POST_SMOOTH | ORCA_INFILL_POST_CLIP | ORCA_INFILL_POST_DROP_SHORT | ORCA_INFILL_POST_CONNECT;

// Copies a pattern into a host record; returns the reason it is rejected, or empty. warning receives the parts of the
// description that are ignored.
std::string copy_pattern(const orca_infill_pattern *source, const std::string &plugin, InfillPluginPattern &out, std::string &warning)
{
    if (source == nullptr)
        return "a pattern is missing";
    const orca_infill_pattern p = read_struct(source);
    if (p.struct_size < ORCA_INFILL_V1_SIZE(orca_infill_pattern, generate))
        return "a pattern has an unknown layout";
    if (p.key == nullptr)
        return "a pattern has no token";
    // InfillPatternRegistry::register_runtime() checks the token itself.
    const std::string key = p.key;
    auto reject = [&key](const std::string &reason) { return key + ": " + reason; };
    if (p.generate == nullptr || p.label == nullptr || *p.label == 0)
        return reject("the pattern needs a label and a generate function");
    if (p.slots == 0 || (p.slots & ~uint32_t(ORCA_INFILL_SLOT_SPARSE | ORCA_INFILL_SLOT_SURFACE)) != 0)
        return reject("the slots must be ORCA_INFILL_SLOT_SPARSE, ORCA_INFILL_SLOT_SURFACE or both");
    if ((p.post & ORCA_INFILL_POST_CONNECT) && ! (p.post & ORCA_INFILL_POST_CLIP))
        return reject("ORCA_INFILL_POST_CONNECT needs ORCA_INFILL_POST_CLIP");
    if (p.param_count > 32 || (p.param_count > 0 && p.params == nullptr))
        return reject("a pattern has at most 32 parameters");
    out = InfillPluginPattern();
    for (size_t i = 0; i < p.param_count; ++ i) {
        if (p.params[i] == nullptr)
            return reject("a parameter is missing");
        const orca_infill_param param = read_struct(p.params[i]);
        if (std::string reason = check_param(param, out.params); ! reason.empty())
            return reject(reason);
        out.params.emplace_back(copy_param(param));
    }
    if ((p.caps & ~known_caps) != 0 || (p.post & ~known_post) != 0)
        warning = reject("unknown capability or processing flags are ignored");
    out.key      = key;
    out.label    = p.label;
    out.plugin   = plugin;
    out.caps     = p.caps & known_caps;
    out.post     = p.post & known_post;
    out.user     = p.user;
    out.generate = p.generate;
    out.slots    = p.slots;
    return {};
}

uint32_t pattern_flags(const InfillPluginPattern &pattern)
{
    uint32_t flags = 0;
    if (pattern.caps & ORCA_INFILL_CAP_MULTILINE)
        flags |= ipfMultiline;
    if (pattern.caps & ORCA_INFILL_CAP_SEPARABLE)
        flags |= ipfSeparable;
    if (pattern.caps & ORCA_INFILL_CAP_ROTATE_TEMPLATE_SAFE)
        flags |= ipfRotateTemplateSafe;
    if (pattern.post & ORCA_INFILL_POST_SMOOTH)
        flags |= ipfSmoothable;
    // The anchor settings have no effect on a pattern whose lines the host does not connect.
    if (! (pattern.post & ORCA_INFILL_POST_CONNECT))
        flags |= ipfNoAnchors;
    return flags;
}

void append_error(std::string &error, const std::string &message)
{
    if (! error.empty())
        error += "; ";
    error += message;
}

// Calls the entry of an opened library and registers its patterns into load. Returns false for a library without the
// entry, which is closed again.
bool register_plugin(Library lib, InfillPluginLoad &load)
{
    const auto entry = reinterpret_cast<orca_infill_plugin_entry_v1_fn>(find_symbol(lib, ORCA_INFILL_ENTRY_V1));
    if (entry == nullptr) {
        // OrcaSlicer took nothing from the library, so it holds no reference into it.
        close_library(lib);
        load.error = "not an infill plugin: it does not export " ORCA_INFILL_ENTRY_V1;
        return false;
    }
    s_libraries.push_back(lib);
    const orca_infill_plugin *returned = nullptr;
    t_loading = &load.path;
    try {
        returned = entry(&s_host);
    } catch (...) {
        t_loading = nullptr;
        load.error = "the entry point of the plugin threw an exception";
        return true;
    }
    t_loading = nullptr;
    if (returned == nullptr) {
        load.error = "the plugin declined OrcaSlicer (infill plugin ABI " + std::to_string(ORCA_INFILL_ABI_MAJOR) + "." +
                     std::to_string(ORCA_INFILL_ABI_MINOR) + ")";
        return true;
    }
    const orca_infill_plugin plugin = read_struct(returned);
    if (plugin.abi_major != ORCA_INFILL_ABI_MAJOR) {
        load.error = "the plugin was built for infill plugin ABI " + std::to_string(plugin.abi_major) + ", OrcaSlicer supports " +
                     std::to_string(ORCA_INFILL_ABI_MAJOR);
        return true;
    }
    if (plugin.struct_size < ORCA_INFILL_V1_SIZE(orca_infill_plugin, pattern_count) || plugin.name == nullptr || plugin.version == nullptr || plugin.pattern_count > 64 ||
        (plugin.pattern_count > 0 && plugin.patterns == nullptr)) {
        load.error = "the plugin description is invalid: it needs a name, a version and at most 64 patterns";
        return true;
    }
    load.plugin = std::string(plugin.name) + " " + plugin.version;

    std::vector<InfillPluginPattern> accepted;
    for (size_t i = 0; i < plugin.pattern_count; ++ i) {
        InfillPluginPattern pattern;
        std::string         warning;
        if (std::string reason = copy_pattern(plugin.patterns[i], load.plugin, pattern, warning); ! reason.empty())
            append_error(load.error, reason);
        else
            accepted.emplace_back(std::move(pattern));
        if (! warning.empty())
            append_error(load.error, warning);
    }
    // Registered in token order, so the values do not depend on the order a plugin lists its patterns in.
    std::sort(accepted.begin(), accepted.end(), [](const InfillPluginPattern &a, const InfillPluginPattern &b) { return a.key < b.key; });
    for (InfillPluginPattern &pattern : accepted) {
        InfillPluginPattern &record = s_patterns.emplace_back(std::move(pattern));
        InfillPatternDescriptor descriptor { ipCount, record.key.c_str(), record.label.c_str(),
                                             uint8_t(((record.slots & ORCA_INFILL_SLOT_SPARSE) ? ipsSparse : 0) |
                                                     ((record.slots & ORCA_INFILL_SLOT_SURFACE) ? ipsSurface : 0)),
                                             pattern_flags(record), 1., 0.5f, false, ipCount, nullptr, &make_plugin_fill };
        descriptor.context = &record;
        std::string error;
        if (const InfillPatternDescriptor *registered = InfillPatternRegistry::register_runtime(descriptor, error); registered != nullptr) {
            record.id = registered->id;
            load.patterns.emplace_back(record.key);
        } else {
            append_error(load.error, record.key + ": " + error);
            s_patterns.pop_back();
        }
    }
    return true;
}

// Loads the file load.path into load without reporting the outcome. Returns the canonical path of a library that does
// not export the entry, and an empty path otherwise.
fs::path load_file(InfillPluginLoad &load)
{
    boost::system::error_code ec;
    const fs::path canonical = fs::canonical(path_from_utf8(load.path), ec);
    if (ec) {
        load.error = "cannot load the library: " + ec.message();
        return {};
    }
    // A file loaded before reports neither a plugin nor an error.
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
    return register_plugin(lib, load) ? fs::path() : canonical;
}

std::string describe(const InfillPluginLoad &load)
{
    return load.error.empty() ? "Loaded infill plugin " + load.plugin + " from " + load.path + ", patterns: " + boost::algorithm::join(load.patterns, ", ") :
                                "Infill plugin " + load.path + ": " + load.error;
}

// Logs and keeps the outcome of a load; returns false when there is nothing to report.
bool report(const InfillPluginLoad &load)
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

// ---- Parameter text ----

const InfillPluginPattern* plugin_pattern(InfillPattern pattern)
{
    if (int(pattern) < int(ipCount))
        return nullptr;
    const InfillPatternDescriptor &descriptor = infill_pattern(pattern);
    return descriptor.plugin() ? static_cast<const InfillPluginPattern*>(descriptor.context) : nullptr;
}

std::string format_value(const InfillPluginParam &param, double value)
{
    switch (param.type) {
    case ORCA_INFILL_PARAM_INT:  return std::to_string(int64_t(value));
    case ORCA_INFILL_PARAM_BOOL: return value != 0. ? "1" : "0";
    case ORCA_INFILL_PARAM_ENUM: return param.enum_values[size_t(value)];
    default:                     return float_to_string_decimal_point(value);
    }
}

// The value of text, or nullopt when it is not a valid value of param.
std::optional<double> parse_value(const InfillPluginParam &param, const std::string &text)
{
    switch (param.type) {
    case ORCA_INFILL_PARAM_BOOL:
        if (text == "1" || text == "true")
            return 1.;
        if (text == "0" || text == "false")
            return 0.;
        return std::nullopt;
    case ORCA_INFILL_PARAM_ENUM: {
        const auto it = std::find(param.enum_values.begin(), param.enum_values.end(), text);
        return it == param.enum_values.end() ? std::nullopt : std::optional<double>(double(it - param.enum_values.begin()));
    }
    default: {
        size_t consumed = 0;
        const double value = string_to_double_decimal_point(text, &consumed);
        if (text.empty() || consumed != text.size() || ! std::isfinite(value))
            return std::nullopt;
        return param.type == ORCA_INFILL_PARAM_INT ? std::round(value) : value;
    }
    }
}

} // namespace

// ---- Host side of FillPlugin ----

Fill* make_plugin_fill(const InfillPatternDescriptor &pattern)
{
    return new FillPlugin(*static_cast<const InfillPluginPattern*>(pattern.context));
}

orca_infill_status call_infill_plugin(const InfillPluginPattern &pattern, const orca_infill_request &request, Polylines &out)
{
    const InfillPluginPattern *previous = t_generating;
    t_generating = &pattern;
    // An exception from a plugin that breaks the ABI rules ends the slice with an error naming the plugin.
    orca_infill_status status = ORCA_INFILL_ERR_EXCEPTION;
    try {
        status = pattern.generate(pattern.user, &s_host, &request, reinterpret_cast<orca_infill_sink*>(&out));
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Infill plugin " << pattern.plugin << " " << pattern.key << " threw: " << e.what();
    } catch (...) {
    }
    t_generating = previous;
    return status;
}

// ---- Loading ----

InfillPluginStartup infill_plugin_startup(int argc, char **argv, const boost::filesystem::path &program)
{
    InfillPluginStartup out;
    std::string data_dir;
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
        if (token == "no-infill-plugins") {
            out.disabled = ! has_value || value == "1" || boost::iequals(value, "true") || boost::iequals(value, "yes") ||
                           boost::iequals(value, "on") || boost::iequals(value, "enabled");
        } else if (token == "infill-plugin-dir" || token == "datadir") {
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

    if (const char *env = boost::nowide::getenv("ORCA_INFILL_PLUGINS"); env != nullptr && std::string(env) == "0")
        out.disabled = true;
    if (const char *env = boost::nowide::getenv("ORCA_INFILL_PLUGIN_PATH"); env != nullptr) {
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
                out.dirs.emplace_back(paths.substr(begin, end - begin));
            begin = end + 1;
        }
    }

    const fs::path exe_dir = program.parent_path();
#if defined(_WIN32)
    out.dirs.emplace_back(path_to_utf8(exe_dir / "infill_plugins"));
#elif defined(__APPLE__)
    // OrcaSlicer.app/Contents/MacOS/<exe> -> OrcaSlicer.app/Contents/PlugIns/infill_plugins
    out.dirs.emplace_back(path_to_utf8(exe_dir.parent_path() / "PlugIns" / "infill_plugins"));
#else
    // <prefix>/bin/<exe> -> <prefix>/lib/orca-slicer/infill_plugins, also inside an AppImage.
    out.dirs.emplace_back(path_to_utf8(exe_dir.parent_path() / "lib" / "orca-slicer" / "infill_plugins"));
#endif
    if (! data_dir.empty())
        out.dirs.emplace_back(path_to_utf8(path_from_utf8(data_dir) / "infill_plugins"));
    return out;
}

std::vector<InfillPluginLoad> load_infill_plugins(const std::vector<std::string> &dirs)
{
    std::vector<InfillPluginLoad> out;
    if (s_disabled || InfillPatternRegistry::sealed())
        return out;
    for (const std::string &dir : dirs) {
        boost::system::error_code ec;
        const fs::path canonical = fs::canonical(path_from_utf8(dir), ec);
        if (ec || ! fs::is_directory(canonical, ec)) {
            BOOST_LOG_TRIVIAL(debug) << "Infill plugin directory " << dir << " does not exist";
            continue;
        }
        if (! s_scanned_dirs.insert(canonical).second)
            continue;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(canonical, ec), end; ! ec && it != end; it.increment(ec))
            if (is_plugin_file_name(it->path()) && fs::is_regular_file(it->path(), ec))
                files.emplace_back(it->path());
        if (ec)
            BOOST_LOG_TRIVIAL(error) << "Infill plugin directory " << dir << " cannot be read: " << ec.message();
        std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) { return a.filename() < b.filename(); });
        // A library without the entry can be a dependency of a plugin next to it. It is reported only when nothing
        // keeps it loaded once the whole directory is loaded.
        std::vector<std::pair<InfillPluginLoad, fs::path>> without_entry;
        for (const fs::path &file : files) {
            InfillPluginLoad load;
            load.path = path_to_utf8(file);
            if (fs::path library = load_file(load); ! library.empty())
                without_entry.emplace_back(std::move(load), std::move(library));
            else if (report(load))
                out.emplace_back(std::move(load));
        }
        for (auto &[load, library] : without_entry)
            if (library_loaded(library))
                BOOST_LOG_TRIVIAL(debug) << "Infill plugin directory " << dir << ": " << load.path << " is a library that another one loaded";
            else if (report(load))
                out.emplace_back(std::move(load));
    }
    return out;
}

InfillPluginLoad load_infill_plugin(const std::string &path)
{
    InfillPluginLoad load;
    load.path = path;
    if (s_disabled || InfillPatternRegistry::sealed()) {
        load.error = "infill plugins are disabled";
        return load;
    }
    load_file(load);
    report(load);
    return load;
}

void disable_infill_plugins() { s_disabled = true; }

const std::vector<InfillPluginLoad>& infill_plugin_failures() { return s_failures; }

void log_infill_plugin_loads()
{
    for (const InfillPluginLoad &load : s_loads)
        BOOST_LOG_TRIVIAL(info) << describe(load);
}

// ---- Parameter text ----

std::vector<double> infill_plugin_param_values(InfillPattern pattern, const std::string &text, std::vector<std::string> *messages)
{
    const InfillPluginPattern *plugin = plugin_pattern(pattern);
    if (plugin == nullptr)
        return {};
    std::vector<double> values;
    for (const InfillPluginParam &param : plugin->params)
        values.push_back(param.default_value);
    if (plugin->params.empty() && messages == nullptr)
        return values;

    // Entries are separated by new lines or ';' and written as <pattern token>/<parameter name> = <value>. The token
    // contains dots and the name none, so the first '/' separates them.
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find_first_of("\n\r;", begin);
        if (end == std::string::npos)
            end = text.size();
        const std::string entry = boost::algorithm::trim_copy(text.substr(begin, end - begin));
        begin = end + 1;
        if (entry.empty())
            continue;
        const size_t      slash = entry.find('/');
        const std::string token = slash == std::string::npos ? std::string() : boost::algorithm::trim_copy(entry.substr(0, slash));
        // Every plugin token has a '.', so an entry without one belongs to no pattern, installed or not.
        if (token.find('.') == std::string::npos) {
            if (messages != nullptr)
                messages->emplace_back(Slic3r::format(_u8L("Infill pattern parameters: \"%1%\" is not written as <pattern>/<parameter> = <value> and is ignored."),
                                                      entry));
            continue;
        }
        if (token != plugin->key)
            continue;
        const size_t      equals = entry.find('=', slash);
        const std::string name   = boost::algorithm::trim_copy(entry.substr(slash + 1, equals == std::string::npos ? std::string::npos : equals - slash - 1));
        const std::string value  = equals == std::string::npos ? std::string() : boost::algorithm::trim_copy(entry.substr(equals + 1));
        const auto        it     = std::find_if(plugin->params.begin(), plugin->params.end(),
                                                [&name](const InfillPluginParam &param) { return param.name == name; });
        if (it == plugin->params.end()) {
            if (messages != nullptr) {
                std::string names;
                for (const InfillPluginParam &param : plugin->params)
                    names += (names.empty() ? "" : ", ") + param.name;
                messages->emplace_back(Slic3r::format(_u8L("Infill pattern %1%: unknown parameter \"%2%\" is ignored. Its parameters are: %3%."),
                                                      plugin->key, name, names));
            }
            continue;
        }
        const InfillPluginParam &param = *it;
        double                  &out   = values[size_t(it - plugin->params.begin())];
        if (const std::optional<double> parsed = parse_value(param, value); ! parsed) {
            out = param.default_value;
            if (messages != nullptr)
                messages->emplace_back(Slic3r::format(_u8L("Infill pattern %1%: \"%2%\" is not a valid value of %3%; %4% is used."),
                                                      plugin->key, value, param.name, format_value(param, out)));
        } else if (*parsed < param.min_value || *parsed > param.max_value) {
            out = std::clamp(*parsed, param.min_value, param.max_value);
            if (messages != nullptr)
                messages->emplace_back(Slic3r::format(_u8L("Infill pattern %1%: %2% = %3% is outside %4% to %5%; %6% is used."),
                                                      plugin->key, param.name, value, format_value(param, param.min_value),
                                                      format_value(param, param.max_value), format_value(param, out)));
        } else
            out = *parsed;
    }
    return values;
}

std::string infill_plugin_params_section(InfillPattern pattern, const std::string &text)
{
    if (int(pattern) < int(ipCount))
        return {};
    const InfillPluginPattern *plugin = plugin_pattern(pattern);
    if (plugin == nullptr || plugin->params.empty())
        return {};
    const std::vector<double> values = infill_plugin_param_values(pattern, text);
    std::string out;
    for (size_t i = 0; i < values.size(); ++ i)
        out += (i == 0 ? "" : ";") + plugin->params[i].name + "=" + float_to_string_decimal_point(values[i]);
    return out;
}

std::string infill_plugin_param_help(InfillPattern pattern)
{
    const InfillPluginPattern *plugin = plugin_pattern(pattern);
    if (plugin == nullptr)
        return {};
    std::string out;
    for (const InfillPluginParam &param : plugin->params) {
        std::string range;
        switch (param.type) {
        case ORCA_INFILL_PARAM_BOOL: range = "0 or 1"; break;
        case ORCA_INFILL_PARAM_ENUM: range = boost::algorithm::join(param.enum_values, ", "); break;
        default: range = format_value(param, param.min_value) + " to " + format_value(param, param.max_value); break;
        }
        out += (out.empty() ? "" : "\n") + plugin->key + "/" + param.name + " = " + format_value(param, param.default_value) +
               (param.unit.empty() ? "" : " " + param.unit) + "  (" + range + ")  " + param.label +
               (param.tooltip.empty() ? "" : ": " + param.tooltip);
    }
    return out;
}

} // namespace Slic3r
