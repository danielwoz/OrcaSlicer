#pragma once

#include <string>
#include <vector>

#include "../PrintConfig.hpp"

namespace Slic3r {

// The parameter values of an infill pattern of a plugin or a module, in declaration order: for a module pattern from
// the region's module settings, for a plugin pattern from the region's infill_plugin_params. Defaults are used where
// there is no valid value, and values are clamped to the declared range. messages receives one translated line for
// each value that is ignored or corrected.
std::vector<double> infill_plugin_param_values(InfillPattern pattern, const PrintRegionConfig *config, std::vector<std::string> *messages = nullptr);
// The same for a plugin pattern, read from an infill_plugin_params text.
std::vector<double> infill_plugin_param_values(InfillPattern pattern, const std::string &text, std::vector<std::string> *messages = nullptr);
// The parameter values of the pattern as a canonical string; empty for a built-in pattern or one without parameters.
std::string infill_plugin_params_section(InfillPattern pattern, const PrintRegionConfig *config);
// One "<token>/<name> = <default>  <range or values>  <label>" line per parameter of the pattern.
std::string infill_plugin_param_help(InfillPattern pattern);
// Whether the pattern is one of an infill plugin, whose parameters are written in infill_plugin_params. The parameters
// of a module's pattern are settings of the module.
bool infill_plugin_uses_text_params(InfillPattern pattern);

} // namespace Slic3r
