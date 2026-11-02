# Writing an OrcaSlicer module

A module is a small shared library (`.dll`, `.so` or `.dylib`) that adds features to OrcaSlicer without changes to
OrcaSlicer itself. It talks to OrcaSlicer through a plain C interface and never links against it; it is native code,
unlike the Python plugins of the *Others* page. A module can add:

- **settings** that work like built-in ones: in presets and projects, per object, part, modifier and height range, on
  the *Modules* page of the print settings, and on the command line;
- **infill patterns**, written as for [infill plugins](../infill/README.md); their parameters become settings;
- a **fill hook**, called for every layer after its infill is made, to trim the infill and add paths to it;
- a **G-code hook**, called for every layer in print order, to insert G-code before and after the layer.

## The short version

```cpp
#include "orca_module_sdk.hpp"

namespace {
struct Hello
{
    static constexpr const char *key     = "hello";      // settings are named hello__<name>; never change it
    static constexpr const char *label   = "Hello";      // title of the settings group
    static constexpr const char *version = "1.0.0";
    static constexpr orca_infill_param from = orca_infill::float_param("from", "From height", 0., 0., 1000., "mm");
    static constexpr orca_module_option options[] = { orca_mod::print_option(from) };

    static void gcode_layer(const orca_mod::GCodeLayer &layer, orca_mod::Text &before, orca_mod::Text &)
    {   // once per pass, before the first layer at or above the height
        if (const double z = layer.option(0); z > 0. && layer.print_z() >= z && layer.previous_print_z() < z)
            before.line("M117 Hello");
    }
};
} // namespace
ORCA_MODULE(Hello)
```

Build it with the CMake helper (include paths, hidden symbols and, on Windows, a static C++ runtime). Users then set
`hello__from` on the Modules page, or `--hello--from=5` on the command line.

```cmake
include(<OrcaSlicer>/sdk/modules/OrcaModule.cmake)
orca_add_module(hello hello.cpp)
```

## Where OrcaSlicer looks

Once, at start-up, in this order: `--module-dir <dir>`; the directories in `ORCA_MODULE_PATH`; `modules` next to the
executable (Windows), in `OrcaSlicer.app/Contents/PlugIns` (macOS) or in `<prefix>/lib/orca-slicer` (Linux); and
`modules` in the data directory. `--no-modules` or `ORCA_MODULES=0` turns modules off. The names of infill plugins
(`--infill-plugin-dir`, `ORCA_INFILL_PLUGIN_PATH`, `infill_plugins`) work too. Modules are never unloaded.

## Settings

- `orca_mod::region_option(value, steps, condition, flags)` has a value per region, `orca_mod::print_option(value,
  condition, flags, steps = ORCA_STEP_GCODE)` one per print; values are `float_param`, `int_param`, `bool_param` or
  `enum_param`, as for infill plugins.
- `steps` is what a change recomputes: `ORCA_STEP_GCODE`, `_INFILL`, `_PREPARE_INFILL`, `_PERIMETERS` or `_SLICE`.
- `condition` greys a setting out while it is false, or hides it with `ORCA_OPTION_HIDE_WHEN_DISABLED`. It is written
  like a custom G-code condition over print settings and your own: `"sparse_infill_density > 0 and hello__from > 0"`.
  Printer and filament settings are not available. A typo rejects the module at start-up, with a message in the log.
- `ORCA_OPTION_ADVANCED` shows a setting in advanced mode only. Hooks get the values in declaration order
  (`option(i)`), clamped to their range; choices as their index, booleans as 0 or 1.
- A project opened without your module keeps your settings, and gives them back once the module is installed again.

## Fill hook

`static void fill_layer(const orca_mod::FillLayer &layer, orca_mod::FillEdit &edit)` runs once per layer of each
object, on several layers at once.

- `layer.regions()` lists the regions with your option values, the sparse density and the line spacings, and
  `layer.areas(region, ORCA_FILL_SPARSE)` the areas of a kind of fill (also `_SOLID`, `_TOP`, `_BOTTOM`).
- `edit.erase(ORCA_FILL_MASK(kind), areas, keep)` removes the fills of those kinds (bridges are of none) inside `areas`
  and outside `keep`, also paths you added before in the same call: add yours after the last erase.
- `edit.add(region, kind, points, closed, height)` adds a path printed like that kind of fill: same speed, flow,
  filament and preview colour. `edit.offset(area, mm)` grows or shrinks an area; coordinates are `layer.units(mm)`.

## G-code hook

`static void gcode_layer(const orca_mod::GCodeLayer &layer, orca_mod::Text &before, orca_mod::Text &after)` runs once
per layer in print order, per object when printing by object. `line()` inserts a line before the layer's extrusions
(after its layer change and own setup, such as the second layer's temperatures) or after the layer, which the preview
and time estimate see; never a comment OrcaSlicer reads back (`append_text` in `orca_module.h`). Region settings hold
the print's values. `previous_print_z()` is -1 on the first layer of a pass.

## Rules of the road

- **Defaults change nothing.** With every setting at its default, your module must not change what OrcaSlicer prints.
- **No state.** Hooks run concurrently, for different layers and prints; a call's output depends only on its arguments.
- **Failures stop the slice.** A hook that throws (`orca_infill::Failed`) stops it with an error naming your module.
- **Keep names stable.** The key and the setting names are stored in projects and presets.
- **The full contract**, with the versioning rules, is in the comments of `include/orca_module.h`.

## Samples

- `samples/infill_walls`: loops around sparse infill, on which its lines end; at a modifier's edge, the neighbour's
  lines lose up to one line spacing. With combined infill, the loops print at the combined height.
- `samples/change_at_z`: a nozzle temperature and a flow from a height on; the flow is reset after each pass. Printing
  by object, an object printed after one that reached the height starts with the changed temperature.
- `../infill/samples/lines` with `ORCA_SAMPLE_LINES_MODULE` defined: the sample pattern as a module, whose parameters
  are the settings `sample_lines__spacing_scale`, `__cross` and `__phase`.
