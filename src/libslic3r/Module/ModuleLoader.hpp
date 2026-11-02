#pragma once

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// Outcome of loading one module or infill plugin file.
struct ModuleLoad
{
    std::string              path;
    std::string              module;    // key of the module; empty for an infill plugin
    std::string              plugin;    // "<name> <version>" of an infill plugin, "<label> <version>" of a module
    std::vector<std::string> patterns;  // tokens of the infill patterns registered from it
    std::vector<std::string> options;   // keys of the settings registered from it
    std::string              error;     // why the library, or some of its parts, were rejected; empty when all loaded
};

// Module directories and switches, read before the command line is parsed, because module settings and patterns can
// be named on the command line.
struct ModuleStartup
{
    // --module-dir values, then ORCA_MODULE_PATH, the bundled directories and those of --datadir. --infill-plugin-dir,
    // ORCA_INFILL_PLUGIN_PATH and the infill_plugins directories are read as well.
    std::vector<std::string> dirs;
    // --no-modules or ORCA_MODULES=0; --no-infill-plugins or ORCA_INFILL_PLUGINS=0 too.
    bool                     disabled { false };
};
ModuleStartup module_startup(int argc, char **argv, const boost::filesystem::path &program);
// The directories of a data directory that modules load from.
std::vector<std::string> module_data_dirs(const std::string &data_dir);

// Loads the modules and infill plugins found directly in the directories, in file name order, and registers what they
// provide. Directories and files already loaded in this process are skipped, and so are libraries without an entry
// point that stay loaded as dependencies of other libraries. Main thread only, before configurations are read on
// other threads; does nothing after disable_modules() or ModuleRegistry::seal().
std::vector<ModuleLoad> load_modules(const std::vector<std::string> &dirs);
// Loads one module or infill plugin file.
ModuleLoad load_module(const std::string &path);
// Turns loading off for the rest of the process.
void disable_modules();
// The loads of this process that reported an error, in load order.
const std::vector<ModuleLoad>& module_load_failures();
// Writes the outcome of every load so far to the log at info level, for the loads made before the log was set up.
void log_module_loads();

} // namespace Slic3r
