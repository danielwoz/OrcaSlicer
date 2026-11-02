#include "orca_module_sdk.hpp"

#include <cstdio>
#include <string>

namespace {

// From a print height on, sets another nozzle temperature and flow.
struct ChangeAtZ
{
    static constexpr const char *key     = "change_at_z";
    static constexpr const char *label   = "Change at height";
    static constexpr const char *version = "1.0.0";

    static constexpr orca_infill_param height = orca_infill::float_param("height", "Height", 0., 0., 2000., "mm",
        "Print height from which the changes below apply. 0 turns this off.");
    // The unit is written as UTF-8 bytes, so it reads the same whatever encoding the compiler assumes for the source.
    static constexpr orca_infill_param temperature = orca_infill::int_param("nozzle_temperature", "Nozzle temperature", 0, 0, 500,
        "\xE2\x84\x83" /* degrees Celsius */,
        "Nozzle temperature of the active extruder from that height; 0 keeps it. A tool change sets its own temperature.");
    static constexpr orca_infill_param flow = orca_infill::int_param("flow", "Flow", 100, 50, 150, "%",
        "Flow from that height, set with M221.");
    static constexpr orca_module_option options[] = {
        orca_mod::print_option(height),
        orca_mod::print_option(temperature, "change_at_z__height > 0"),
        orca_mod::print_option(flow, "change_at_z__height > 0"),
    };
    enum { Height, Temperature, Flow };

    static void gcode_layer(const orca_mod::GCodeLayer &layer, orca_mod::Text &before, orca_mod::Text &after)
    {
        const double     z   = layer.option(Height);
        const int        f   = int(layer.option(Flow));
        constexpr double eps = 1e-6;
        if (z <= 0. || layer.print_z() < z - eps)
            return;
        // Once per pass: before the first layer at or above the height.
        if (layer.previous_print_z() < z - eps) {
            // snprintf keeps the decimal point whatever the locale of C++ streams.
            char line[64];
            std::snprintf(line, sizeof(line), "; change_at_z: from %.3f mm", z);
            before.line(line);
            if (const int t = int(layer.option(Temperature)); t > 0)
                before.line("M104 S" + std::to_string(t));
            if (f != 100)
                before.line("M221 S" + std::to_string(f));
        }
        // The printer keeps the flow factor after the print, so it is reset after the last layer of the pass.
        if (f != 100 && layer.layer_index() == layer.layer_count() - 1)
            after.line("M221 S100");
    }
};

} // namespace

ORCA_MODULE(ChangeAtZ)
