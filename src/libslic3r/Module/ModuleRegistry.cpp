#include "ModuleRegistry.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include <orca_module.h>

#include "../I18N.hpp"
#include "../LocalesUtils.hpp"
#include "../Model.hpp"
#include "../PlaceholderParser.hpp"
#include "../Preset.hpp"
#include "../Print.hpp"
#include "../format.hpp"
#include "../Fill/FillPlugin.hpp"
#include "../Fill/InfillPatternRegistry.hpp"

namespace Slic3r {

static_assert(msGCode == ORCA_STEP_GCODE && msInfill == ORCA_STEP_INFILL && msPrepareInfill == ORCA_STEP_PREPARE_INFILL &&
              msPerimeters == ORCA_STEP_PERIMETERS && msSlice == ORCA_STEP_SLICE, "ModuleStep follows ORCA_STEP_*");

namespace {

constexpr uint32_t all_steps     = msGCode | msInfill | msPrepareInfill | msPerimeters | msSlice;
constexpr uint32_t known_flags   = ORCA_OPTION_ADVANCED | ORCA_OPTION_HIDE_WHEN_DISABLED;
constexpr size_t   max_condition = 1024;

// ---- State of this process: written on the main thread before seal(), read anywhere afterwards. Deques keep the
// addresses of their elements while they grow. ----

bool                                                s_sealed = false;
std::deque<ModuleRecord>                            s_records;
std::deque<orca_module>                             s_abi;
std::vector<const ModuleRecord*>                    s_modules;   // by key
std::deque<ModuleOption>                            s_options;
std::map<std::string, const ModuleOption*>          s_options_by_key;
std::vector<const ModuleOption*>                    s_conditional;
std::vector<std::unique_ptr<t_config_enum_values>>  s_enum_maps;
bool                                                s_fill_hooks = false;

// ^[a-z][a-z0-9]*(_[a-z0-9]+)*$, at most 24 characters, so it never contains "__".
bool valid_module_key(const char *key)
{
    if (key == nullptr)
        return false;
    const size_t length = std::strlen(key);
    if (length == 0 || length > 24 || ! (key[0] >= 'a' && key[0] <= 'z') || key[length - 1] == '_')
        return false;
    for (size_t i = 1; i < length; ++ i) {
        const char c = key[i];
        if (! ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '_' && key[i - 1] != '_')))
            return false;
    }
    return true;
}

ConfigOptionType option_type(int32_t type)
{
    switch (type) {
    case ORCA_INFILL_PARAM_FLOAT: return coFloat;
    case ORCA_INFILL_PARAM_INT:   return coInt;
    case ORCA_INFILL_PARAM_BOOL:  return coBool;
    default:                      return coEnum;
    }
}

ConfigOption* default_option(const InfillPluginParam &value, const t_config_enum_values *map)
{
    switch (value.type) {
    case ORCA_INFILL_PARAM_FLOAT: return new ConfigOptionFloat(value.default_value);
    case ORCA_INFILL_PARAM_INT:   return new ConfigOptionInt(int(value.default_value));
    case ORCA_INFILL_PARAM_BOOL:  return new ConfigOptionBool(value.default_value != 0.);
    default:                      return new ConfigOptionEnumGeneric(map, int(value.default_value));
    }
}

std::unique_ptr<t_config_enum_values> enum_map(const InfillPluginParam &value)
{
    if (value.type != ORCA_INFILL_PARAM_ENUM)
        return nullptr;
    auto map = std::make_unique<t_config_enum_values>();
    for (size_t i = 0; i < value.enum_values.size(); ++ i)
        map->emplace(value.enum_values[i], int(i));
    return map;
}

// A setting checked and ready to be registered.
struct PendingSetting
{
    std::string                           key;
    InfillPluginParam                     value;
    std::unique_ptr<t_config_enum_values> map;
    ModuleScope                           scope { ModuleScope::Region };
    uint32_t                              invalidates { 0 };
    bool                                  advanced { false }, hide_when_disabled { false };
    std::string                           enabled_if;
};

// Defines the setting in print_config_def, the command line definition and the print preset keys.
const ModuleOption& register_setting(ModuleRecord &record, PendingSetting &&setting)
{
    ConfigOptionDef *def = PrintConfigDef::add_module_option(setting.key, option_type(setting.value.type));
    assert(def != nullptr);
    def->label    = setting.value.label;
    def->tooltip  = setting.value.tooltip;
    def->sidetext = setting.value.unit;
    def->category = record.label;
    def->mode     = setting.advanced ? comAdvanced : comSimple;
    if (setting.value.type == ORCA_INFILL_PARAM_FLOAT || setting.value.type == ORCA_INFILL_PARAM_INT) {
        def->min = float(setting.value.min_value);
        def->max = float(setting.value.max_value);
    } else if (setting.value.type == ORCA_INFILL_PARAM_ENUM) {
        def->enum_values   = setting.value.enum_values;
        def->enum_labels   = setting.value.enum_labels;
        def->enum_keys_map = s_enum_maps.emplace_back(std::move(setting.map)).get();
    }
    def->set_default_value(default_option(setting.value, def->enum_keys_map));
    DynamicPrintAndCLIConfig::add_module_option(*def);
    Preset::append_print_options({ setting.key });

    ModuleOption &option = s_options.emplace_back();
    option.key                = std::move(setting.key);
    option.enabled_if         = std::move(setting.enabled_if);
    option.def                = def;
    option.scope              = setting.scope;
    option.invalidates        = setting.invalidates;
    option.hide_when_disabled = setting.hide_when_disabled;
    option.min_value          = setting.value.min_value;
    option.max_value          = setting.value.max_value;
    option.module             = &record;
    s_options_by_key.emplace(option.key, &option);
    if (! option.enabled_if.empty())
        s_conditional.emplace_back(&option);
    record.settings.emplace_back(&option);
    return option;
}

// Checks the options of a module; returns the reason the module is rejected, or empty. warning receives the parts of
// the options that are ignored.
std::string check_options(const orca_module &module, std::vector<PendingSetting> &out, std::string &warning)
{
    std::vector<InfillPluginParam> values;
    for (size_t i = 0; i < module.option_count; ++ i) {
        if (module.options[i] == nullptr)
            return "an option is missing";
        const orca_module_option option = read_abi_struct(module.options[i]);
        if (option.struct_size < ORCA_ABI_V1_SIZE(orca_module_option, enabled_if))
            return "an option has an unknown layout";
        if (option.value == nullptr)
            return "an option has no value";
        const orca_infill_param param = read_abi_struct(option.value);
        if (std::string reason = check_infill_param(param, values); ! reason.empty())
            return reason;
        PendingSetting &setting = out.emplace_back();
        setting.value = copy_infill_param(param);
        setting.key   = std::string(module.key) + "__" + setting.value.name;
        values.emplace_back(setting.value);
        if (option.scope != ORCA_SCOPE_PRINT && option.scope != ORCA_SCOPE_REGION)
            return "option " + setting.key + " has an unknown scope";
        if (option.invalidates == 0 || (option.invalidates & ~all_steps) != 0)
            return "option " + setting.key + " must invalidate one or more of the ORCA_STEP_* steps";
        if (print_config_def.has(setting.key))
            return "option " + setting.key + " is already defined";
        if (option.enabled_if != nullptr && std::strlen(option.enabled_if) > max_condition)
            return "the condition of option " + setting.key + " is longer than " + std::to_string(max_condition) + " bytes";
        if ((option.flags & ~known_flags) != 0)
            append_error(warning, "option " + setting.key + ": unknown flags are ignored");
        setting.map                = enum_map(setting.value);
        setting.scope              = option.scope == ORCA_SCOPE_PRINT ? ModuleScope::Print : ModuleScope::Region;
        setting.invalidates        = option.invalidates;
        setting.advanced           = (option.flags & ORCA_OPTION_ADVANCED) != 0;
        setting.hide_when_disabled = (option.flags & ORCA_OPTION_HIDE_WHEN_DISABLED) != 0;
        setting.enabled_if         = option.enabled_if != nullptr ? option.enabled_if : "";
    }
    return {};
}

// Evaluates each condition against the default print settings and the module's options at their defaults; returns the
// reason the module is rejected, or empty.
std::string check_conditions(const std::vector<PendingSetting> &settings)
{
    static const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();
    DynamicConfig own;
    for (const PendingSetting &setting : settings)
        own.set_key_value(setting.key, default_option(setting.value, setting.map.get()));
    for (const PendingSetting &setting : settings)
        if (! setting.enabled_if.empty())
            try {
                PlaceholderParser::evaluate_boolean_expression(setting.enabled_if, defaults, &own);
            } catch (const std::exception &e) {
                // The parser's message goes on with the expression and a marker on lines of their own.
                const std::string message = e.what();
                return "the condition of option " + setting.key + " is invalid: " + message.substr(0, message.find('\n'));
            }
    return {};
}

// The condition under which the parameters of a pattern are used: one of the pattern options of its slots names it.
std::string pattern_condition(const InfillPluginPattern &pattern)
{
    const uint8_t slots = uint8_t(((pattern.slots & ORCA_INFILL_SLOT_SPARSE) ? ipsSparse : 0) |
                                  ((pattern.slots & ORCA_INFILL_SLOT_SURFACE) ? ipsSurface : 0));
    std::vector<std::string> terms;
    for (const auto &[opt_key, slot] : InfillPatternRegistry::options())
        if (slot & slots)
            terms.emplace_back(std::string(opt_key) + " == \"" + pattern.key + "\"");
    return boost::algorithm::join(terms, " or ");
}

// Copies, checks and registers the patterns of a module; their parameters become region settings of the module.
void register_patterns(const orca_module &module, ModuleRecord &record, std::string &error)
{
    std::vector<InfillPluginPattern> accepted;
    std::set<std::string>            parameter_keys;
    for (size_t i = 0; i < module.pattern_count; ++ i) {
        InfillPluginPattern pattern;
        std::string         warning;
        if (std::string reason = copy_infill_pattern(module.patterns[i], record.label + " " + record.version, pattern, warning); ! reason.empty()) {
            append_error(error, reason);
            continue;
        }
        if (! warning.empty())
            append_error(error, warning);
        const auto taken = std::find_if(pattern.params.begin(), pattern.params.end(), [&record, &parameter_keys](const InfillPluginParam &param) {
            const std::string key = record.key + "__" + param.name;
            return print_config_def.has(key) || parameter_keys.count(key) > 0;
        });
        if (taken != pattern.params.end()) {
            append_error(error, pattern.key + ": the setting " + record.key + "__" + taken->name + " of parameter " + taken->name + " is already defined");
            continue;
        }
        for (const InfillPluginParam &param : pattern.params)
            parameter_keys.insert(record.key + "__" + param.name);
        pattern.module     = &record;
        pattern.log_prefix = "Module " + record.key + " " + pattern.key + ": ";
        accepted.emplace_back(std::move(pattern));
    }
    for (InfillPluginPattern *pattern : register_infill_patterns(std::move(accepted), error)) {
        record.patterns.emplace_back(pattern->key);
        const std::string condition = pattern_condition(*pattern);
        for (const InfillPluginParam &param : pattern->params) {
            PendingSetting setting;
            setting.key                = record.key + "__" + param.name;
            setting.value              = param;
            setting.map                = enum_map(param);
            setting.invalidates        = msPrepareInfill;
            setting.hide_when_disabled = true;
            setting.enabled_if         = condition;
            pattern->param_options.emplace_back(&register_setting(record, std::move(setting)));
        }
    }
}

// ---- Bags ----

// Calls visit(key, value) for each "key=value" line of a bag.
template<class Visit> void for_each_line(std::string_view bag, Visit &&visit)
{
    for (size_t begin = 0; begin < bag.size();) {
        size_t end = bag.find('\n', begin);
        if (end == std::string_view::npos)
            end = bag.size();
        const std::string_view line   = bag.substr(begin, end - begin);
        const size_t           equals = line.find('=');
        if (equals != std::string_view::npos)
            visit(line.substr(0, equals), line.substr(equals + 1));
        begin = end + 1;
    }
}

std::map<std::string, std::string> parse_bag(std::string_view bag)
{
    std::map<std::string, std::string> out;
    for_each_line(bag, [&out](std::string_view key, std::string_view value) { out[std::string(key)] = std::string(value); });
    return out;
}

std::optional<std::string_view> find_in_bag(std::string_view bag, std::string_view key)
{
    std::optional<std::string_view> out;
    for_each_line(bag, [&out, key](std::string_view line_key, std::string_view value) {
        if (line_key == key)
            out = value;
    });
    return out;
}

// A value of a setting as a number, ENUM as index and BOOL as 0 or 1.
double number(const ConfigOption &value)
{
    switch (value.type()) {
    case coFloat: return value.getFloat();
    case coBool:  return value.getBool() ? 1. : 0.;
    default:      return double(value.getInt());
    }
}

// The value of text as a number, unclamped; nullopt when it is not a value of the setting.
std::optional<double> parse_value(const ModuleOption &option, std::string_view text)
{
    std::unique_ptr<ConfigOption> value(option.def->default_value->clone());
    return value->deserialize(std::string(text)) ? std::optional<double>(number(*value)) : std::nullopt;
}

bool has_range(const ModuleOption &option) { return option.def->type == coFloat || option.def->type == coInt; }

std::string format_number(const ModuleOption &option, double value)
{
    return option.def->type == coInt ? std::to_string(int64_t(value)) : float_to_string_decimal_point(value);
}

// ---- Kept settings ----

// The lines of missing_module_options by key; lines that are not "key = value" with a module setting key are dropped.
std::map<std::string, std::string> parse_kept(const std::string &text)
{
    std::map<std::string, std::string> out;
    for_each_line(text, [&out](std::string_view key, std::string_view value) {
        std::string k = boost::algorithm::trim_copy(std::string(key));
        if (is_module_option_key(k))
            out[std::move(k)] = boost::algorithm::trim_copy(std::string(value));
    });
    return out;
}

void store_kept(DynamicPrintConfig &config, const std::map<std::string, std::string> &kept)
{
    if (kept.empty() && ! config.has("missing_module_options"))
        return;
    std::string text;
    for (const auto &[key, value] : kept)
        text += (text.empty() ? "" : "\n") + key + " = " + value;
    config.option<ConfigOptionString>("missing_module_options", true)->value = std::move(text);
}

std::map<std::string, std::string> kept_of(const DynamicPrintConfig &config)
{
    const ConfigOptionString *kept = config.option<ConfigOptionString>("missing_module_options");
    return kept != nullptr ? parse_kept(kept->value) : std::map<std::string, std::string>();
}

} // namespace

bool is_module_option_key(const std::string &key) { return key.find("__") != std::string::npos; }

// ---- Registration ----

const ModuleRecord* ModuleRegistry::register_module(const void *source, std::string &error)
{
    assert(! s_sealed);
    if (s_sealed) {
        error = "module registration is closed";
        return nullptr;
    }
    if (source == nullptr) {
        error = "the module is missing";
        return nullptr;
    }
    const orca_module module = read_abi_struct(static_cast<const orca_module*>(source));
    std::string       reason;
    if (module.struct_size < ORCA_ABI_V1_SIZE(orca_module, gcode_layer))
        reason = "the module description has an unknown layout";
    else if (module.abi_major != ORCA_MODULE_ABI_MAJOR)
        reason = "the module was built for module ABI " + std::to_string(module.abi_major) + ", OrcaSlicer supports " +
                 std::to_string(ORCA_MODULE_ABI_MAJOR);
    else if (! valid_module_key(module.key))
        reason = "the module key must match [a-z][a-z0-9]*(_[a-z0-9]+)* and have at most 24 characters";
    else if (std::any_of(s_records.begin(), s_records.end(), [&module](const ModuleRecord &other) { return other.key == module.key; }))
        reason = "the module key " + std::string(module.key) + " is already in use";
    else if (module.label == nullptr || *module.label == 0 || module.version == nullptr)
        reason = "the module needs a label and a version";
    else if (module.pattern_count > 64 || module.option_count > 64 || (module.pattern_count > 0 && module.patterns == nullptr) ||
             (module.option_count > 0 && module.options == nullptr))
        reason = "a module has at most 64 patterns and at most 64 options";
    std::vector<PendingSetting> settings;
    std::string                 warning;
    if (reason.empty())
        reason = check_options(module, settings, warning);
    if (reason.empty())
        reason = check_conditions(settings);
    if (! reason.empty()) {
        error = valid_module_key(module.key) ? std::string(module.key) + ": " + reason : reason;
        return nullptr;
    }
    error = warning;

    ModuleRecord &record = s_records.emplace_back();
    record.key            = module.key;
    record.label          = module.label;
    record.version        = module.version;
    record.has_fill_hook  = module.fill_layer != nullptr;
    record.has_gcode_hook = module.gcode_layer != nullptr;
    record.abi            = &s_abi.emplace_back(module);
    for (PendingSetting &setting : settings)
        record.options.emplace_back(&register_setting(record, std::move(setting)));
    register_patterns(module, record, error);
    s_fill_hooks |= record.has_fill_hook;
    s_modules.emplace_back(&record);
    std::sort(s_modules.begin(), s_modules.end(), [](const ModuleRecord *a, const ModuleRecord *b) { return a->key < b->key; });
    return &record;
}

void ModuleRegistry::seal()
{
    s_sealed = true;
    InfillPatternRegistry::seal();
}

bool ModuleRegistry::sealed() { return s_sealed; }
const std::vector<const ModuleRecord*>& ModuleRegistry::modules() { return s_modules; }

const ModuleOption* ModuleRegistry::option(const std::string &key)
{
    const auto it = s_options_by_key.find(key);
    return it == s_options_by_key.end() ? nullptr : it->second;
}

std::vector<std::string> ModuleRegistry::option_keys(ModuleScope scope)
{
    std::vector<std::string> out;
    for (const auto &[key, option] : s_options_by_key)
        if (option->scope == scope)
            out.emplace_back(key);
    return out;
}

bool ModuleRegistry::has_options() { return ! s_options.empty(); }
bool ModuleRegistry::has_fill_hooks() { return s_fill_hooks; }

// ---- Values ----

void ModuleRegistry::pack_values(DynamicPrintConfig &full_config)
{
    if (s_options.empty())
        return;
    std::string print_bag, region_bag;
    for (const auto &[key, option] : s_options_by_key)
        if (const ConfigOption *value = full_config.option(key); value != nullptr && ! (*value == *option->def->default_value)) {
            std::string &bag = option->scope == ModuleScope::Print ? print_bag : region_bag;
            bag += (bag.empty() ? "" : "\n") + key + "=" + value->serialize();
        }
    full_config.set_key_value("module_print_values", new ConfigOptionString(std::move(print_bag)));
    full_config.set_key_value("module_region_values", new ConfigOptionString(std::move(region_bag)));
}

void ModuleRegistry::set_bag_value(ConfigOptionString &bag, const ModuleOption &option, const ConfigOption &value)
{
    std::map<std::string, std::string> values = parse_bag(bag.value);
    if (value == *option.def->default_value)
        values.erase(option.key);
    else
        values[option.key] = value.serialize();
    std::string text;
    for (const auto &[key, v] : values)
        text += (text.empty() ? "" : "\n") + key + "=" + v;
    bag.value = std::move(text);
}

double ModuleRegistry::bag_value(const std::string &bag, const ModuleOption &option)
{
    std::optional<double> value;
    if (const std::optional<std::string_view> text = find_in_bag(bag, option.key); text)
        value = parse_value(option, *text);
    const double out = value ? *value : number(*option.def->default_value);
    return has_range(option) ? std::clamp(out, option.min_value, option.max_value) : out;
}

std::vector<double> ModuleRegistry::option_values(const ModuleRecord &module, const PrintConfig &print, const PrintRegionConfig &region)
{
    std::vector<double> out;
    out.reserve(module.options.size());
    for (const ModuleOption *option : module.options)
        out.emplace_back(bag_value(option->scope == ModuleScope::Print ? print.module_print_values.value : region.module_region_values.value, *option));
    return out;
}

uint32_t ModuleRegistry::changed_steps(const std::string &old_bag, const std::string &new_bag)
{
    const std::map<std::string, std::string> old_values = parse_bag(old_bag), new_values = parse_bag(new_bag);
    uint32_t steps = 0;
    auto changed = [&steps](const std::string &key) {
        const ModuleOption *option = ModuleRegistry::option(key);
        steps |= option != nullptr ? option->invalidates : all_steps;
    };
    for (const auto &[key, value] : old_values)
        if (const auto it = new_values.find(key); it == new_values.end() || it->second != value)
            changed(key);
    for (const auto &[key, value] : new_values)
        if (old_values.count(key) == 0)
            changed(key);
    return steps;
}

bool ModuleRegistry::is_default_value(const std::string &key, const ConfigOption &value)
{
    const ModuleOption *option = ModuleRegistry::option(key);
    return option != nullptr && value == *option->def->default_value;
}

bool ModuleRegistry::omit_on_save(const std::string &key, const ConfigOption &value)
{
    if (key == "module_print_values" || key == "module_region_values")
        return true;
    if (key == "missing_module_options")
        return value.type() == coString && static_cast<const ConfigOptionString&>(value).value.empty();
    return is_default_value(key, value);
}

// ---- Kept settings ----

bool ModuleRegistry::keep_unknown_option(DynamicPrintConfig &config, const std::string &key, const std::string &value,
                                         ConfigSubstitutionContext &substitutions)
{
    if (key == "module_print_values" || key == "module_region_values")
        return true;
    if (key == "missing_module_options") {
        // Decoded as a string option's value is: files other than JSON store it C-escaped, its line breaks as "\n".
        std::string text;
        if (! unescape_string_cstyle(value, text))
            text = value;
        std::map<std::string, std::string> kept = kept_of(config);
        for (auto &[k, v] : parse_kept(text)) {
            if (ModuleRegistry::option(k) == nullptr)
                kept[k] = std::move(v);
            else if (! config.has(k))
                // A value its module no longer accepts is dropped.
                config.set_deserialize_nothrow(k, v, substitutions);
        }
        store_kept(config, kept);
        return true;
    }
    // A line break would split the line of the setting.
    if (! is_module_option_key(key) || value.find_first_of("\r\n") != std::string::npos)
        return false;
    std::map<std::string, std::string> kept = kept_of(config);
    kept[key] = boost::algorithm::trim_copy(value);
    store_kept(config, kept);
    return true;
}

// ---- Conditions ----

const std::vector<const ModuleOption*>& ModuleRegistry::options_with_condition() { return s_conditional; }

bool ModuleRegistry::condition_holds(const ModuleOption &option, const DynamicConfig &config)
{
    if (option.enabled_if.empty())
        return true;
    try {
        return PlaceholderParser::evaluate_boolean_expression(option.enabled_if, config);
    } catch (const std::exception &e) {
        static std::mutex                     mutex;
        static std::set<const ModuleOption*>  logged;
        const std::lock_guard<std::mutex>     lock(mutex);
        if (logged.insert(&option).second)
            BOOST_LOG_TRIVIAL(warning) << "Module setting " << option.key << " stays enabled: its condition \"" << option.enabled_if
                                       << "\" cannot be evaluated: " << e.what();
        return true;
    }
}

// ---- Warnings ----

std::vector<ModuleWarning> ModuleRegistry::warnings(const Print &print)
{
    std::vector<ModuleWarning> out;
    auto warn_kept = [&out](const DynamicPrintConfig &config, const ObjectBase *object) {
        const std::map<std::string, std::string> kept = kept_of(config);
        if (kept.empty())
            return;
        std::vector<std::string> keys;
        for (const auto &[key, value] : kept)
            keys.emplace_back(key);
        out.push_back({ Slic3r::format(_u8L("These settings belong to modules that are not installed, or that no longer define them. "
                                            "They are kept in the project but not used: %1%."),
                                       boost::algorithm::join(keys, ", ")),
                        object, "missing_module_options" });
    };
    warn_kept(print.full_print_config(), nullptr);
    // A ModelObject has a PrintObject per distinct instance rotation; it is reported once.
    std::set<const ModelObject*> reported;
    for (const PrintObject *object : print.objects()) {
        const ModelObject &model_object = *object->model_object();
        if (! reported.insert(&model_object).second)
            continue;
        warn_kept(model_object.config.get(), object);
        for (const ModelVolume *volume : model_object.volumes)
            warn_kept(volume->config.get(), object);
        for (const auto &range : model_object.layer_config_ranges)
            warn_kept(range.second.get(), object);
    }

    if (s_options.empty())
        return out;
    std::set<std::pair<std::string, std::string>> reported_values;
    auto warn_range = [&out, &reported_values](const std::string &bag, const ObjectBase *object) {
        for_each_line(bag, [&](std::string_view key, std::string_view text) {
            const ModuleOption *option = ModuleRegistry::option(std::string(key));
            if (option == nullptr || ! has_range(*option))
                return;
            const std::optional<double> value = parse_value(*option, text);
            if (! value || (*value >= option->min_value && *value <= option->max_value) ||
                ! reported_values.emplace(std::string(key), std::string(text)).second)
                return;
            out.push_back({ Slic3r::format(_u8L("%1% = %2% is outside %3% to %4%; %5% is used."), option->key, std::string(text),
                                           format_number(*option, option->min_value), format_number(*option, option->max_value),
                                           format_number(*option, std::clamp(*value, option->min_value, option->max_value))),
                            object, option->key });
        });
    };
    warn_range(print.config().module_print_values.value, nullptr);
    for (const PrintObject *object : print.objects())
        for (size_t i = 0; i < object->num_printing_regions(); ++ i)
            warn_range(object->printing_region(i).config().module_region_values.value, object);
    return out;
}

} // namespace Slic3r
