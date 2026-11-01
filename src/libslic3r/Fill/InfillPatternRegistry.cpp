#include "InfillPatternRegistry.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "../Exception.hpp"

namespace Slic3r {
namespace detail {

std::string build_infill_pattern_table(const InfillModuleEntry *entries, size_t count, InfillPatternTable &table)
{
    table = InfillPatternTable();
    table.rows.resize(size_t(ipCount));
    table.row_module.assign(size_t(ipCount), nullptr);
    std::vector<std::pair<InfillPatternDescriptor, const InfillModule*>> appended;
    for (size_t i = 0; i < count; ++ i) {
        const InfillModule &module = *entries[i].module;
        const std::string   name   = module.name;
        table.modules.emplace_back(&module);
        for (size_t j = 0; j < module.pattern_count; ++ j) {
            InfillPatternDescriptor row = module.patterns[j];
            row.optional = ! entries[i].core;
            if (! entries[i].built)
                // The module's sources are not part of this build.
                row.factory = nullptr;
            else if (row.factory == nullptr)
                return "a pattern of the module " + name + " has no factory";
            if (entries[i].core != (row.fallback == ipCount))
                return "a pattern of the module " + name + (entries[i].core ? " is always built but names a fallback" : " may be left out but names no fallback");
            if (row.appended()) {
                if (row.internal())
                    return "a pattern of the module " + name + " has neither an enum value nor a token";
                appended.emplace_back(row, &module);
            } else if (const InfillModule *other = table.row_module[size_t(row.id)]; other != nullptr)
                return "the modules " + std::string(other->name) + " and " + name + " both describe InfillPattern " + std::to_string(int(row.id));
            else {
                table.rows[size_t(row.id)]       = row;
                table.row_module[size_t(row.id)] = &module;
            }
        }
        for (size_t j = 0; j < module.region_option_count; ++ j)
            table.options.emplace_back(&module.region_options[j], &module);
    }
    for (size_t id = 0; id < size_t(ipCount); ++ id)
        if (table.row_module[id] == nullptr)
            return "no module describes InfillPattern " + std::to_string(id);
    // Appended patterns follow the enum values in token order, whatever order the modules are listed in.
    std::sort(appended.begin(), appended.end(), [](const auto &a, const auto &b) { return std::strcmp(a.first.key, b.first.key) < 0; });
    for (auto &[row, module] : appended) {
        row.id = InfillPattern(table.rows.size());
        table.rows.emplace_back(row);
        table.row_module.emplace_back(module);
    }
    for (InfillPatternDescriptor &row : table.rows) {
        const std::string name = row.internal() ? "InfillPattern " + std::to_string(int(row.id)) : std::string(row.key);
        if (! row.optional)
            row.fallback = row.id;
        else if (unsigned(row.fallback) >= unsigned(ipCount) || table.rows[size_t(row.fallback)].optional ||
                 (row.slots & ~table.rows[size_t(row.fallback)].slots) != 0)
            return "the fallback of " + name + " must be an enum value of an always built module offered in all of its slots";
        if (row.menu_after != nullptr) {
            // The enum values come first, so their tokens are already known here.
            const auto anchor = table.by_key.find(row.menu_after);
            if (anchor == table.by_key.end() || int(anchor->second) >= int(ipCount))
                return "menu_after of " + name + " must name a pattern with an enum value";
        }
        if (! row.internal() && ! table.by_key.emplace(row.key, row.id).second)
            return "the token " + name + " is used twice";
        table.all_available = table.all_available && row.available();
    }
    std::set<std::string> option_keys;
    for (const auto &[option, module] : table.options)
        if (! option_keys.insert(option->key).second)
            return std::string("the option ") + option->key + " is added twice";
    return {};
}

std::vector<const InfillPatternDescriptor*> infill_slot_patterns(const InfillPatternTable &table, InfillPatternSlot slot)
{
    std::vector<const InfillPatternDescriptor*> out;
    // The enum values in their order, then the appended patterns without a place, in token order.
    for (const InfillPatternDescriptor &pattern : table.rows)
        if ((pattern.slots & slot) && pattern.menu_after == nullptr)
            out.emplace_back(&pattern);
    // Each pattern with a place goes right after its anchor; going backwards keeps several of them in token order.
    for (auto it = table.rows.rbegin(); it != table.rows.rend(); ++ it)
        if ((it->slots & slot) && it->menu_after != nullptr) {
            auto anchor = std::find_if(out.begin(), out.end(), [&it](const InfillPatternDescriptor *p) { return std::strcmp(p->key, it->menu_after) == 0; });
            out.insert(anchor == out.end() ? out.end() : anchor + 1, &*it);
        }
    return out;
}

} // namespace detail

namespace {

const detail::InfillPatternTable& table()
{
    // Built on first use, which is the initialisation of s_keys_map_InfillPattern in PrintConfig.cpp. It reads only
    // the module objects and the generated list, which are constant-initialised.
    static const detail::InfillPatternTable s_table = [] {
        detail::InfillPatternTable t;
        if (const std::string error = detail::build_infill_pattern_table(detail::infill_module_entries, detail::infill_module_entry_count, t);
            ! error.empty()) {
            // The modules of this build contradict each other; no configuration could be read reliably.
            std::fprintf(stderr, "OrcaSlicer infill pattern modules: %s\n", error.c_str());
            std::abort();
        }
        return t;
    }();
    return s_table;
}

// The descriptor of a value that names no pattern: void area factor 1, combine clearance factor 0.5.
constexpr InfillPatternDescriptor s_none { ipCount, nullptr, nullptr, 0, 0, 1., 0.5f, ipCount, nullptr, nullptr };

constexpr std::array<std::pair<const char*, InfillPatternSlot>, 6> s_options {{
    { "sparse_infill_pattern", ipsSparse }, { "top_surface_pattern", ipsSurface }, { "bottom_surface_pattern", ipsSurface },
    { "internal_solid_infill_pattern", ipsSurface }, { "ironing_pattern", ipsIroning }, { "support_ironing_pattern", ipsIroning } }};

} // namespace

bool InfillPatternDescriptor::has_option(const char *opt_key) const
{
    for (const char *const *it = options; it != nullptr && *it != nullptr; ++ it)
        if (std::strcmp(*it, opt_key) == 0)
            return true;
    return false;
}

const std::array<std::pair<const char*, InfillPatternSlot>, 6>& InfillPatternRegistry::options() { return s_options; }
const std::vector<InfillPatternDescriptor>& InfillPatternRegistry::patterns() { return table().rows; }
InfillPatternRange InfillPatternRegistry::builtins() { return { table().rows.data(), size_t(ipCount) }; }

const InfillPatternDescriptor* InfillPatternRegistry::find(InfillPattern id)
{
    const std::vector<InfillPatternDescriptor> &rows = table().rows;
    return unsigned(id) < rows.size() ? &rows[size_t(id)] : nullptr;
}

const InfillPatternDescriptor* InfillPatternRegistry::find(const std::string &key)
{
    const std::map<std::string, InfillPattern> &by_key = table().by_key;
    const auto it = by_key.find(key);
    return it == by_key.end() ? nullptr : find(it->second);
}

t_config_enum_values InfillPatternRegistry::keys_map()
{
    t_config_enum_values keys;
    for (const InfillPatternDescriptor &pattern : table().rows)
        if (! pattern.internal())
            keys.emplace(pattern.key, int(pattern.id));
    return keys;
}

void InfillPatternRegistry::define_enum_values(ConfigOptionDef &def, InfillPatternSlot slot)
{
    def.enum_values.clear();
    def.enum_labels.clear();
    for (const InfillPatternDescriptor *pattern : detail::infill_slot_patterns(table(), slot)) {
        def.enum_values.emplace_back(pattern->key);
        def.enum_labels.emplace_back(pattern->label);
    }
}

Fill* InfillPatternRegistry::create(InfillPattern id)
{
    const InfillPatternDescriptor &pattern = sliced_infill_pattern(id);
    if (pattern.factory == nullptr)
        throw Slic3r::InvalidArgument("unknown type");
    return pattern.factory(pattern);
}

bool InfillPatternRegistry::all_available() { return table().all_available; }

void InfillPatternRegistry::resolve_unavailable(PrintRegionConfig &config)
{
    if (all_available())
        return;
    for (ConfigOptionEnum<InfillPattern> *opt : { &config.sparse_infill_pattern, &config.top_surface_pattern, &config.bottom_surface_pattern,
                                                  &config.internal_solid_infill_pattern, &config.ironing_pattern })
        if (const InfillPatternDescriptor *pattern = find(opt->value); pattern != nullptr && ! pattern->available())
            opt->value = pattern->fallback;
}

std::vector<std::pair<std::string, const InfillPatternDescriptor*>> InfillPatternRegistry::unavailable_in(const ConfigBase &config)
{
    std::vector<std::pair<std::string, const InfillPatternDescriptor*>> out;
    for (const auto &[opt_key, slot] : s_options)
        if (const ConfigOption *opt = config.option(opt_key); opt != nullptr)
            if (const InfillPatternDescriptor *pattern = find(InfillPattern(opt->getInt())); pattern != nullptr && ! pattern->available())
                out.emplace_back(opt_key, pattern);
    return out;
}

const std::vector<const InfillModule*>& InfillPatternRegistry::modules() { return table().modules; }

const InfillModule* InfillPatternRegistry::module_of(InfillPattern id)
{
    const std::vector<const InfillModule*> &row_module = table().row_module;
    return unsigned(id) < row_module.size() ? row_module[size_t(id)] : nullptr;
}

const std::vector<std::pair<const InfillRegionOption*, const InfillModule*>>& InfillPatternRegistry::region_options() { return table().options; }

const InfillRegionOption* InfillPatternRegistry::region_option(const std::string &opt_key)
{
    for (const auto &[option, module] : table().options)
        if (opt_key == option->key)
            return option;
    return nullptr;
}

void InfillPatternRegistry::define_region_options(const InfillOptionAdder &add)
{
    for (const InfillModule *module : table().modules)
        if (module->define_region_options != nullptr)
            module->define_region_options(add);
}

void InfillPatternRegistry::module_params(InfillPattern id, const PrintRegionConfig &config, std::string &out)
{
    out.clear();
    const InfillPatternDescriptor *pattern = find(id);
    if (pattern == nullptr)
        return;
    for (const char *const *key = pattern->options; key != nullptr && *key != nullptr; ++ key)
        if (const ConfigOption *option = config.option(*key);
            option != nullptr && (pattern->appended() || region_option(*key) != nullptr)) {
            out += option->serialize();
            out += '\n';
        }
}

const InfillPatternDescriptor& infill_pattern(InfillPattern pattern)
{
    const InfillPatternDescriptor *found = InfillPatternRegistry::find(pattern);
    return found == nullptr ? s_none : *found;
}

const InfillPatternDescriptor& sliced_infill_pattern(InfillPattern pattern)
{
    const InfillPatternDescriptor *found = InfillPatternRegistry::find(pattern);
    return found == nullptr ? s_none : found->available() ? *found : *InfillPatternRegistry::find(found->fallback);
}

// The infill pattern traits declared in PrintConfig.hpp; they describe the pattern that is sliced.
bool is_separable_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfSeparable); }
bool is_octree_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfOctree); }
bool is_adaptive_infill_pattern(InfillPattern pattern) { return sliced_infill_pattern(pattern).has(ipfAdaptiveDensity); }
bool is_smoothable_infill_pattern(InfillPattern pattern, int multiline) { return sliced_infill_pattern(pattern).smoothable(multiline); }

} // namespace Slic3r
