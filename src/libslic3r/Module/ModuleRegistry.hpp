#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../PrintConfig.hpp"

namespace Slic3r {

class ObjectBase;
class Print;

// What a change of a module setting recomputes, a step and every later one; the ORCA_STEP_* bits of orca_module.h.
enum ModuleStep : uint32_t { msGCode = 1, msInfill = 2, msPrepareInfill = 4, msPerimeters = 8, msSlice = 16 };
enum class ModuleScope { Print, Region };

struct ModuleRecord;

// A setting a module registered: one of its options, or a parameter of one of its infill patterns.
struct ModuleOption
{
    std::string            key;          // "<module>__<name>"
    std::string            enabled_if;   // empty: always
    const ConfigOptionDef *def { nullptr };
    ModuleScope            scope { ModuleScope::Region };
    uint32_t               invalidates { 0 };   // ModuleStep bits
    bool                   hide_when_disabled { false };
    double                 min_value { 0. }, max_value { 0. };   // FLOAT and INT: the declared range
    const ModuleRecord    *module { nullptr };
};

// A loaded module; lives until the process exits.
struct ModuleRecord
{
    std::string                      key, label, version;
    std::vector<const ModuleOption*> options;   // the module's own, in declaration order (the hooks' index space)
    std::vector<const ModuleOption*> settings;  // options, then pattern parameters: what the GUI shows
    std::vector<std::string>         patterns;  // tokens of its infill patterns
    bool                             has_fill_hook { false }, has_gcode_hook { false };
    const void                      *abi { nullptr };   // host copy of the orca_module
};

// A slicing warning about module settings.
struct ModuleWarning
{
    std::string       message;
    const ObjectBase *object { nullptr };
    std::string       opt_key;
};

// The modules of this process, their settings and the values of those settings. Registration happens on the main
// thread before any configuration is read on another thread; everything else may run on any thread after seal().
class ModuleRegistry
{
public:
    // Registers a module (an orca_module); main thread, before seal(). Returns nullptr when the module is rejected;
    // error lists what was rejected, the module or some of its patterns.
    static const ModuleRecord* register_module(const void *orca_module, std::string &error);
    // Closes registration, of modules and of infill patterns.
    static void seal();
    static bool sealed();
    // The modules by key.
    static const std::vector<const ModuleRecord*>& modules();
    // nullptr: not a registered module setting.
    static const ModuleOption* option(const std::string &key);
    // Keys of the registered settings of a scope, in key order.
    static std::vector<std::string> option_keys(ModuleScope scope);
    static bool has_options();
    static bool has_fill_hooks();

    // ---- Values. A bag (module_print_values, module_region_values) holds a "key=value" line for each setting of its
    // scope whose value differs from the default, sorted by key; empty when every setting is at its default. ----

    // Sets the bags of full_config from the module settings in it; settings absent from it count as defaults. Leaves
    // full_config alone while no module has settings.
    static void pack_values(DynamicPrintConfig &full_config);
    // Sets, or removes when it equals the default, the value of a setting in a bag.
    static void set_bag_value(ConfigOptionString &bag, const ModuleOption &option, const ConfigOption &value);
    // The value of a setting in a bag, clamped to its range, ENUM as index and BOOL as 0 or 1; the default when the bag
    // has none.
    static double bag_value(const std::string &bag, const ModuleOption &option);
    // The values of every option of a module, in declaration order, from the bag of each option's scope.
    static std::vector<double> option_values(const ModuleRecord &module, const PrintConfig &print, const PrintRegionConfig &region);
    // The ModuleStep bits of the settings whose values differ between two bags; every bit for a setting no loaded
    // module defines.
    static uint32_t changed_steps(const std::string &old_bag, const std::string &new_bag);
    // Whether key is a registered module setting and value its default.
    static bool is_default_value(const std::string &key, const ConfigOption &value);
    // Whether configuration files leave a key out: a module setting at its default, an empty missing_module_options
    // or a bag. Reading a file fills them in from the defaults, so a build without the module keeps nothing for it.
    static bool omit_on_save(const std::string &key, const ConfigOption &value);

    // ---- Settings of modules that are not loaded, kept in missing_module_options as "key = value" lines ----

    // Deals with a key that handle_legacy() dropped while config was read: keeps the value of a setting no loaded
    // module defines, restores the kept values of loaded modules, and swallows the bags. Returns false for any other key.
    static bool keep_unknown_option(DynamicPrintConfig &config, const std::string &key, const std::string &value,
                                    ConfigSubstitutionContext &substitutions);

    // ---- Conditions ----

    // The settings that have an enabled_if condition, by module and in declaration order.
    static const std::vector<const ModuleOption*>& options_with_condition();
    // Whether the condition of a setting holds in config. A condition that cannot be evaluated holds, and is logged once.
    static bool condition_holds(const ModuleOption &option, const DynamicConfig &config);

    // ---- Warnings ----

    // The settings kept for modules that are not loaded, and module values outside their ranges, in a print.
    static std::vector<ModuleWarning> warnings(const Print &print);
};

// Whether a configuration key names a module setting, loaded or not: no built-in key contains "__".
bool is_module_option_key(const std::string &key);

} // namespace Slic3r
