# Infill pattern modules — High Level Design

## Purpose and scope

Every infill pattern is one row of the infill pattern registry
(`src/libslic3r/Fill/InfillPatternRegistry.hpp`): its token, label, the pattern
options that offer it (slots), the flags that code outside its `Fill` class
reads, its fallback, the options shown only while it is selected, and the
factory of its filler. The rows come from modules. A module is a directory
`src/libslic3r/Fill/Modules/<name>/`, or a directory named in the CMake cache
variable `ORCA_INFILL_MODULE_DIRS`, so adding a pattern adds one directory and
edits no shared file.

## A module

A module directory holds two files, plus any sources of its own:

- `module.cmake` calls `orca_add_infill_module()` once. Its arguments are
  documented in `src/libslic3r/Fill/Modules/InfillModules.cmake`: the module
  name, the module source, the sources compiled only when the module is built,
  whether a build may leave it out (`CORE OFF`), and the settings it adds
  (`REGION_OPTIONS`).
- The module source defines its rows in a `constexpr` array and the module
  object with `ORCA_INFILL_MODULE(<name>, rows, std::size(rows), ...)`, at
  namespace scope inside `namespace Slic3r`. The macro checks every row at
  compile time and requires constant arguments, so the object is ready before
  any dynamic initialiser reads it.

CMake globs `Fill/Modules/*/module.cmake` and generates three files:
`infill_modules.cpp`, which names every module object so the static library
keeps it; `libslic3r_infill_patterns.h`, with `ORCA_INFILL_<NAME>` set to 1 or 0
for each optional module; and `libslic3r_infill_module_options.h`, with the
members and keys the modules add to `PrintRegionConfig`. Visual Studio and
Xcode need a CMake run to see a new directory.

The registry builds its table on first use, while `PrintConfig.cpp` initialises
its statics. Rules that span modules are checked there: each enum value belongs
to one module, tokens and option keys are unique, fallbacks are always built,
and `menu_after` names a pattern with an enum value. A contradiction stops every
binary at start with `OrcaSlicer infill pattern modules: ...`, so no test run
passes with it.

## Built-in patterns and new patterns

The 23 in-tree modules describe the 31 values of the `InfillPattern` enum. Their
filler classes stay in `src/libslic3r/Fill/`, and each module source names the
file. The options these patterns own are defined in `PrintConfig.cpp`.

A new pattern has no enum value: its row says `ipCount`, and the registry
numbers such patterns from `ipCount` on, in token order. Configurations, 3MF
projects and presets store the token, never the number. Such a pattern:

- is offered for sparse infill, for surfaces, or both;
- is listed after the built-in token its `menu_after` names, or after all
  built-in patterns;
- names a fallback: an enum value of an always built module, offered in every
  slot of the pattern, that is sliced when a build leaves the module out;
- reads its settings from `params.config`, the region's `PrintRegionConfig`,
  including the bridge anchoring pass.

A module that a build may leave out and that compiles its filler from its own
sources creates it through `optional_fill<ORCA_INFILL_<NAME> != 0, T>()`.
`&make_fill<T>` would need the class even in a build without those sources, and
the link would fail.

## Settings of a module

A setting added by a module is written in four places, and the registry tests
check that they agree:

1. `REGION_OPTIONS` in `module.cmake` gives its type and key, which becomes a
   `PrintRegionConfig` member. The key starts with `<name>_` and must not be an
   option of the slicer.
2. The module's `InfillRegionOption` array gives the slicing step a change
   invalidates and the help page of the settings line.
3. The module's define function sets the label, tooltip, limits and default.
   The definition is in category Strength, mode Advanced.
4. The `options` list of the pattern that owns it shows the setting only while
   that pattern is selected.

These settings are print preset options. They are saved in 3MF projects, set
from the CLI, listed in the Infill group after the settings of the built-in
patterns, and offered as per-object Strength settings. Regions
whose values differ are filled apart: `InfillPatternRegistry::module_params`
adds the values to the key that groups fill regions. A setting exists even in
a build that leaves its module out, so configurations keep their values.

## Builds without a module

`-DORCA_INFILL_<NAME>=OFF` leaves an optional module out, and
`-DORCA_INFILL_OPTIONAL_DEFAULT=OFF` starts every optional module off. The module
source is still compiled, so the pattern stays known: a configuration naming it
loads, the pattern lists show it as not included in this build, slicing uses its
fallback, and `Print::validate` warns.

## Translations and icons

The gettext scripts scan the sources under `src/libslic3r/Fill/Modules`, so
labels and tooltips marked with `L()` there need no entry in
`localization/i18n/list.txt`. A pattern list shows the icon
`resources/images/param_<token>.svg` when it exists.

## Worked example

`tests/infill_modules/staggered` is a complete module: a pattern without an enum
value, its own filler, two settings and a place after Line. It is built only on
request, so the shipped build never contains it:

```bash
cmake ... -DORCA_INFILL_MODULE_DIRS=tests/infill_modules/staggered
cmake ... -DORCA_INFILL_MODULE_DIRS=tests/infill_modules/staggered -DORCA_INFILL_STAGGERED=OFF
```

The `[InfillRegistry]` cases of `libslic3r_tests`, the `[InfillModule]` cases of
`fff_print_tests` and the CLI tests `cli_infill_module` and
`cli_infill_unavailable` cover the module built and left out.
