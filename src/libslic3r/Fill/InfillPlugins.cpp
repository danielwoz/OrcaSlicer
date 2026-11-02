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
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r_version.h"
#include "../ClipperUtils.hpp"
#include "../I18N.hpp"
#include "../LocalesUtils.hpp"
#include "../format.hpp"
#include "../Module/ModuleRegistry.hpp"
#include "FillPlugin.hpp"
#include "InfillPatternRegistry.hpp"

namespace Slic3r {
namespace {

// Addresses stay valid while patterns are appended; written on the main thread before the registry is sealed.
std::deque<InfillPluginPattern> s_patterns;

// Starts the log lines of the host services on this thread; set by InfillLogSource.
thread_local const std::string *t_log_prefix = nullptr;

// ---- Host services: callable from any thread, never throw ----

Points to_points(const orca_infill_point *points, size_t count)
{
    Points out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++ i)
        out.emplace_back(coord_t(points[i].x), coord_t(points[i].y));
    return out;
}

bool valid_path(const orca_infill_path &path) { return path.count == 0 || path.points != nullptr; }

orca_infill_status ORCA_INFILL_CALL host_emit_polyline(orca_infill_sink *sink, const orca_infill_point *points, size_t count) noexcept
{
    if (sink == nullptr || points == nullptr || count < 2)
        return ORCA_INFILL_ERR_INVALID;
    return host_service([&] { reinterpret_cast<Polylines*>(sink)->emplace_back(to_points(points, count)); });
}

void ORCA_INFILL_CALL host_log(int32_t level, const char *message) noexcept
{
    try {
        const std::string text = (t_log_prefix != nullptr ? *t_log_prefix : std::string("Infill plugin: ")) + (message != nullptr ? message : "");
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
    if ((polylines == nullptr && count > 0) || ! valid_infill_region(region) || visit == nullptr)
        return ORCA_INFILL_ERR_INVALID;
    return host_service([&] {
        Polylines lines;
        lines.reserve(count);
        for (size_t i = 0; i < count; ++ i)
            if (polylines[i].count >= 2 && polylines[i].points != nullptr)
                lines.emplace_back(to_points(polylines[i].points, polylines[i].count));
        std::vector<orca_infill_point> piece;
        for (const Polyline &line : intersection_pl(lines, infill_region_expolygon(*region))) {
            piece.clear();
            for (const Point &p : line.points)
                piece.push_back({ p.x(), p.y() });
            visit(user, piece.data(), piece.size());
        }
    });
}

orca_infill_status ORCA_INFILL_CALL host_offset_region(const orca_infill_region *region, double delta_mm,
                                                       orca_infill_region_visitor visit, void *user) noexcept
{
    if (! valid_infill_region(region) || ! std::isfinite(delta_mm) || visit == nullptr)
        return ORCA_INFILL_ERR_INVALID;
    return host_service([&] {
        for (const ExPolygon &expolygon : offset_ex(infill_region_expolygon(*region), float(scale_(delta_mm)))) {
            const InfillPluginRegion out(expolygon);
            visit(user, &out.view());
        }
    });
}

const orca_infill_host s_host { sizeof(orca_infill_host), ORCA_INFILL_ABI_MAJOR, ORCA_INFILL_ABI_MINOR, SoftFever_VERSION,
                                &host_emit_polyline, &host_log, &host_clip_polylines, &host_offset_region };

// ---- Validation ----

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

constexpr uint32_t known_caps = ORCA_INFILL_CAP_MULTILINE | ORCA_INFILL_CAP_SEPARABLE | ORCA_INFILL_CAP_FIXED_ANGLE |
                                ORCA_INFILL_CAP_KEEP_ORDER | ORCA_INFILL_CAP_ROTATE_TEMPLATE_SAFE;
constexpr uint32_t known_post = ORCA_INFILL_POST_SMOOTH | ORCA_INFILL_POST_CLIP | ORCA_INFILL_POST_DROP_SHORT | ORCA_INFILL_POST_CONNECT;

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

std::string params_section(const InfillPluginPattern &plugin, const std::vector<double> &values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++ i)
        out += (i == 0 ? "" : ";") + plugin.params[i].name + "=" + float_to_string_decimal_point(values[i]);
    return out;
}

} // namespace

// ---- Host services ----

const orca_infill_host& infill_plugin_host() { return s_host; }

ExPolygon infill_region_expolygon(const orca_infill_region &region)
{
    ExPolygon out;
    out.contour.points = to_points(region.contour.points, region.contour.count);
    for (size_t i = 0; i < region.hole_count; ++ i)
        out.holes.emplace_back(to_points(region.holes[i].points, region.holes[i].count));
    return out;
}

bool valid_infill_region(const orca_infill_region *region)
{
    if (region == nullptr || ! valid_path(region->contour) || (region->hole_count > 0 && region->holes == nullptr))
        return false;
    for (size_t i = 0; i < region->hole_count; ++ i)
        if (! valid_path(region->holes[i]))
            return false;
    return true;
}

InfillLogSource::InfillLogSource(const std::string &prefix) : m_previous(t_log_prefix) { t_log_prefix = &prefix; }
InfillLogSource::~InfillLogSource() { t_log_prefix = m_previous; }

// ---- Host side of FillPlugin ----

Fill* make_plugin_fill(const InfillPatternDescriptor &pattern)
{
    return new FillPlugin(*static_cast<const InfillPluginPattern*>(pattern.context));
}

orca_infill_status call_infill_plugin(const InfillPluginPattern &pattern, const orca_infill_request &request, Polylines &out)
{
    return call_guarded(pattern.log_prefix, [&] {
        return pattern.generate(pattern.user, &s_host, &request, reinterpret_cast<orca_infill_sink*>(&out));
    });
}

// ---- Validation and registration ----

std::string check_infill_param(const orca_infill_param &p, const std::vector<InfillPluginParam> &previous)
{
    if (p.struct_size < ORCA_ABI_V1_SIZE(orca_infill_param, enum_count))
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

InfillPluginParam copy_infill_param(const orca_infill_param &p)
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

std::string copy_infill_pattern(const orca_infill_pattern *source, const std::string &plugin, InfillPluginPattern &out, std::string &warning)
{
    if (source == nullptr)
        return "a pattern is missing";
    const orca_infill_pattern p = read_abi_struct(source);
    if (p.struct_size < ORCA_ABI_V1_SIZE(orca_infill_pattern, generate))
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
        const orca_infill_param param = read_abi_struct(p.params[i]);
        if (std::string reason = check_infill_param(param, out.params); ! reason.empty())
            return reject(reason);
        out.params.emplace_back(copy_infill_param(param));
    }
    if ((p.caps & ~known_caps) != 0 || (p.post & ~known_post) != 0)
        warning = reject("unknown capability or processing flags are ignored");
    out.key        = key;
    out.label      = p.label;
    out.plugin     = plugin;
    out.log_prefix = "Infill plugin " + plugin + " " + key + ": ";
    out.caps       = p.caps & known_caps;
    out.post       = p.post & known_post;
    out.user       = p.user;
    out.generate   = p.generate;
    out.slots      = p.slots;
    return {};
}

std::vector<InfillPluginPattern*> register_infill_patterns(std::vector<InfillPluginPattern> patterns, std::string &error)
{
    std::sort(patterns.begin(), patterns.end(), [](const InfillPluginPattern &a, const InfillPluginPattern &b) { return a.key < b.key; });
    std::vector<InfillPluginPattern*> out;
    for (InfillPluginPattern &pattern : patterns) {
        InfillPluginPattern &record = s_patterns.emplace_back(std::move(pattern));
        InfillPatternDescriptor descriptor { ipCount, record.key.c_str(), record.label.c_str(),
                                             uint8_t(((record.slots & ORCA_INFILL_SLOT_SPARSE) ? ipsSparse : 0) |
                                                     ((record.slots & ORCA_INFILL_SLOT_SURFACE) ? ipsSurface : 0)),
                                             pattern_flags(record), 1., 0.5f, false, ipCount, nullptr, &make_plugin_fill };
        descriptor.context = &record;
        std::string reason;
        if (const InfillPatternDescriptor *registered = InfillPatternRegistry::register_runtime(descriptor, reason); registered != nullptr) {
            record.id = registered->id;
            out.emplace_back(&record);
        } else {
            append_error(error, record.key + ": " + reason);
            s_patterns.pop_back();
        }
    }
    return out;
}

void append_error(std::string &error, const std::string &message)
{
    if (! error.empty())
        error += "; ";
    error += message;
}

// ---- Parameters ----

std::vector<double> infill_plugin_param_values(InfillPattern pattern, const PrintRegionConfig *config, std::vector<std::string> *messages)
{
    const InfillPluginPattern *plugin = plugin_pattern(pattern);
    if (plugin == nullptr || plugin->module == nullptr)
        return infill_plugin_param_values(pattern, config != nullptr ? config->infill_plugin_params.value : std::string(), messages);
    // Values outside their range are reported by ModuleRegistry::warnings(), as for every module setting.
    std::vector<double> values;
    for (const ModuleOption *option : plugin->param_options)
        values.push_back(ModuleRegistry::bag_value(config != nullptr ? config->module_region_values.value : std::string(), *option));
    return values;
}

std::vector<double> infill_plugin_param_values(InfillPattern pattern, const std::string &text, std::vector<std::string> *messages)
{
    const InfillPluginPattern *plugin = plugin_pattern(pattern);
    if (plugin == nullptr)
        return {};
    std::vector<double> values;
    for (const InfillPluginParam &param : plugin->params)
        values.push_back(param.default_value);
    if (plugin->module != nullptr || (plugin->params.empty() && messages == nullptr))
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

std::string infill_plugin_params_section(InfillPattern pattern, const PrintRegionConfig *config)
{
    const InfillPluginPattern *plugin = plugin_pattern(pattern);
    return plugin == nullptr || plugin->params.empty() ? std::string() : params_section(*plugin, infill_plugin_param_values(pattern, config));
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

bool infill_plugin_uses_text_params(InfillPattern pattern)
{
    const InfillPluginPattern *plugin = plugin_pattern(pattern);
    return plugin != nullptr && plugin->module == nullptr;
}

} // namespace Slic3r
