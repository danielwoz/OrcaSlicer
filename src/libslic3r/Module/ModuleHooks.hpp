#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct orca_module_host;

namespace Slic3r {

class Layer;
class Print;
struct ModuleRecord;

// The host services of modules; the table lives until the process exits.
const orca_module_host& module_host();

// Calls the fill hooks of the modules, in module key order, on a layer whose fills were just made. Runs on the thread
// that filled the layer and touches only that layer. Throws SlicingError naming the module when a hook fails.
void run_module_fill_hooks(Layer &layer);

// The G-code hook of a module, with the values of its options for one print.
struct ModuleGCodeHook
{
    const ModuleRecord *module { nullptr };
    std::vector<double> options;
};
// The G-code hooks of the modules, in module key order, with their option values for the print.
std::vector<ModuleGCodeHook> module_gcode_hooks(const Print &print);
// Calls the G-code hooks for one layer of a pass and inserts their text into the layer's G-code: the "before" text at
// setup_end, where the layer change and the layer's own setup commands end, the "after" text at its end.
// previous_print_z is -1 for the first layer of a pass. Throws SlicingError naming the module when a hook fails.
void apply_module_gcode_hooks(const std::vector<ModuleGCodeHook> &hooks, std::string &gcode, size_t setup_end, double print_z,
                              double previous_print_z, size_t layer_index, size_t layer_count);

} // namespace Slic3r
