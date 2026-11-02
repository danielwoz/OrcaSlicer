#pragma once

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "../PrintConfig.hpp"

namespace Slic3r {

// Outcome of loading one infill plugin file.
struct InfillPluginLoad
{
    std::string              path;
    std::string              plugin;    // "<name> <version>" as reported by the plugin
    std::vector<std::string> patterns;  // tokens of the patterns registered from it
    std::string              error;     // why the plugin, or some of its patterns, were rejected; empty when all loaded
};

// Plugin directories and switches, read before the command line is parsed, because a pattern of a plugin can be the
// value of a command line option.
struct InfillPluginStartup
{
    // --infill-plugin-dir values, then ORCA_INFILL_PLUGIN_PATH, the bundled directory and <--datadir>/infill_plugins.
    std::vector<std::string> dirs;
    // --no-infill-plugins, or ORCA_INFILL_PLUGINS=0.
    bool                     disabled { false };
};
InfillPluginStartup infill_plugin_startup(int argc, char **argv, const boost::filesystem::path &program);

// Loads the infill plugins found directly in the directories, in file name order, and registers their patterns.
// Directories and files already loaded in this process are skipped, and so are libraries without the entry point that
// stay loaded as dependencies of other libraries. Main thread only, before configurations are read on other threads;
// does nothing after disable_infill_plugins() or InfillPatternRegistry::seal().
std::vector<InfillPluginLoad> load_infill_plugins(const std::vector<std::string> &dirs);
// Loads one plugin file and registers its patterns.
InfillPluginLoad load_infill_plugin(const std::string &path);
// Turns loading off for the rest of the process.
void disable_infill_plugins();
// The loads of this process that reported an error, in load order.
const std::vector<InfillPluginLoad>& infill_plugin_failures();
// Writes the outcome of every load so far to the log at info level, for the loads made before the log was set up.
void log_infill_plugin_loads();

// The parameter values of a plugin pattern, in declaration order, read from an infill_plugin_params text: defaults
// where the text has no valid value, clamped to the declared range. messages receives one translated line for each
// entry of this pattern that is ignored or corrected.
std::vector<double> infill_plugin_param_values(InfillPattern pattern, const std::string &text, std::vector<std::string> *messages = nullptr);
// The parameter values of the pattern as a canonical string; empty for a built-in pattern or one without parameters.
std::string infill_plugin_params_section(InfillPattern pattern, const std::string &text);
// An infill_plugin_params text in canonical form for pattern: its entries replaced by one "<token>/<name> = <value>"
// entry per value that differs from the default, every other entry kept as written, one entry per line, sorted by
// pattern and parameter; repeated entries keep their order, so the last one still wins.
std::string infill_plugin_params_with(const std::string &text, InfillPattern pattern, const std::vector<double> &values);
// "<pattern label>: <name>=<value>, ...", joined by "; ", for the plugin patterns whose values in the text differ from
// their defaults; empty when there are none. changed, when given, receives the number of those values.
std::string infill_plugin_params_summary(const std::string &text, size_t *changed = nullptr);

struct InfillPluginPattern;
// The registered plugin patterns, by token.
std::vector<const InfillPluginPattern*> infill_plugin_patterns();

} // namespace Slic3r
