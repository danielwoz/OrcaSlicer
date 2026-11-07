# Writing an infill pattern plugin

An infill plugin adds patterns to OrcaSlicer's sparse, top, bottom or internal solid infill lists. It is a small shared
library (`.dll`, `.so` or `.dylib`) that talks to OrcaSlicer through a plain C interface, never linking against it.

## The short version

1. Describe your pattern as a C++ struct and draw lines in `generate()`:

   ```cpp
   #include "orca_infill_sdk.hpp"

   namespace {   // keeps the type private to the plugin
   struct Stripes {
       static constexpr const char *key   = "org.example.stripes";   // the pattern token, see below
       static constexpr const char *label = "Stripes";
       static constexpr uint32_t    slots = ORCA_INFILL_SLOT_SPARSE;
       static constexpr uint32_t    caps  = ORCA_INFILL_CAP_MULTILINE;
       static constexpr uint32_t    post  = ORCA_INFILL_POST_DEFAULT;  // clip, drop short pieces, connect
       static constexpr orca_infill_param params[] = { orca_infill::float_param("gap", "Gap scale", 1., 0.5, 4.) };
       static void generate(const orca_infill::Request &req, orca_infill::Output &out) {
           const int64_t step = req.units(req.line_distance() * req.param(0));
           const orca_infill_bbox &box = req.bbox();
           for (int64_t y = orca_infill::Request::align_up(box.min.y, req.origin().y, step); y <= box.max.y; y += step)
               out.line({ box.min.x, y }, { box.max.x, y });
       }
   }; } // namespace
   ORCA_INFILL_PLUGIN("Example patterns", "1.0.0", Stripes)
   ```

2. Build it with a `CMakeLists.txt` like this one; on Windows the helper links the C++ runtime in, so the plugin never
   depends on OrcaSlicer's:
   ```cmake
   cmake_minimum_required(VERSION 3.15)   # 3.15 is needed for the static MSVC runtime
   project(stripes LANGUAGES CXX)
   set(ORCA_INFILL_SDK_DIR "" CACHE PATH "<OrcaSlicer>/sdk/infill")
   if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
       set(CMAKE_BUILD_TYPE Release)
   endif ()
   include(${ORCA_INFILL_SDK_DIR}/OrcaInfillPlugin.cmake)
   orca_add_infill_plugin(stripes stripes.cpp)
   ```
   `cmake -S . -B build -DORCA_INFILL_SDK_DIR=<OrcaSlicer>/sdk/infill && cmake --build build --config Release`
3. Put the library where OrcaSlicer looks, restart, and pick "Stripes" in the pattern list. Libraries it depends on
   can sit next to it (give it an `$ORIGIN` or `@loader_path` rpath on Linux and macOS); linking them in is simpler.

`samples/lines/lines.cpp` is a complete plugin with a float, a bool and a choice parameter.

## Where OrcaSlicer looks

In this order; the first plugin to claim a pattern token wins:

- `--infill-plugin-dir <dir>[;<dir>...]` on the command line;
- the directories in `ORCA_INFILL_PLUGIN_PATH` (`;`-separated on Windows, `:` elsewhere);
- the application's own folder: `infill_plugins` next to the executable on Windows,
  `OrcaSlicer.app/Contents/PlugIns/infill_plugins` on macOS, `<prefix>/lib/orca-slicer/infill_plugins` on Linux;
- `infill_plugins` in the data directory (the GUI's, or the CLI's `--datadir`).

Each directory is read without its subdirectories, so point it at the folder holding the library itself; a
multi-configuration generator puts it in `build/Release`. Only `.dll` files are loaded on Windows, `.so` on Linux,
and `.so` or `.dylib` on macOS. `--no-infill-plugins` or `ORCA_INFILL_PLUGINS=0` turns plugins off.

Problems are logged and shown as a notification. `out.log()` writes to the same log. On the command line,
`--debug 3 --logfile <file>` writes info lines, including "Loaded infill plugin ...", to that file. A
`--sparse-infill-pattern` value naming a pattern whose plugin was not found is rejected as an invalid value.

## How your lines are used

- `generate()` is called once per region of a layer, in the *pattern frame*: the infill direction is the x axis, so
  draw your lines as if the infill angle were zero. Lay them out from `req.origin()` so they line up across regions
  and layers. Coordinates are integers; use `req.units(mm)` and `req.mm(units)`, never a fixed scale. `req.units()`
  rounds to the nearest unit, where libslic3r's `scale_()` cast truncates: keep that in mind when porting a pattern
  and comparing G-code exactly.
- `req.layer_index()` counts raft layers. `req.line_distance()` is the distance between lines for the requested
  density, already allowing for multiline; `req.multiline()` is 1 unless the pattern declares
  `ORCA_INFILL_CAP_MULTILINE`.
- `caps` says which slicer settings apply to the pattern: `MULTILINE` (`fill_multiline`; the host draws the copies),
  `SEPARABLE` (`separated_infills` moves the origin to each body), `FIXED_ANGLE` (no 90 degree turn between layers),
  `KEEP_ORDER` (paths print in the order emitted) and `ROTATE_TEMPLATE_SAFE` (`sparse_infill_rotate_template`).
- Draw across the whole `req.bbox()`. `ORCA_INFILL_POST_CLIP` cuts the lines to the region (without it they must stay
  inside); `ORCA_INFILL_POST_DEFAULT` also drops pieces under 0.8 x spacing and joins line ends along the outline.
- Parameters arrive in `req.param(i)`, in declaration order; choices arrive as their index. Users set them in the
  *Pattern parameters* setting, one per line or separated by `;`: `org.example.stripes/gap = 2`, also on the command
  line as `--infill-plugin-params "org.example.stripes/gap = 2"`. On/off values accept `1`/`0` and `true`/`false`;
  values out of range are clamped and unknown names are reported, both with a warning.
- Throw `orca_infill::Failed` when a region cannot be filled: it stays empty. Any other exception stops the slice
  with an error naming your plugin.
- Need help with geometry? `out.clip()` and `out.offset()` take `req.region()` or a region `out.offset()` returned.

## Rules of the road

- `generate()` runs on several threads at once. Keep it free of shared mutable state, do not start thread pools,
  and make the output depend only on the request, so the same model always slices the same way.
- Never let an exception or `longjmp` leave a plugin function; the SDK's `ORCA_INFILL_PLUGIN` takes care of this.
- A pattern token is two or more parts matching `[a-z0-9][a-z0-9_-]*` joined by `.`, at most 64 characters. Keep
  tokens, parameter names and choice values stable: projects and presets store them. A project that uses your
  pattern opens without the plugin with the default pattern instead, and keeps the parameters.
- The library is loaded once and never unloaded. On macOS, an unsigned download may need
  `xattr -d com.apple.quarantine <file>` before it loads.

## OrcaSlicer's own patterns

The built-in patterns are libraries too, one per pattern, named `orca_infill_<token>` (`orca_infill_gyroid.dll`,
`orca_infill_tri-hexagon.so`, ...; two internal patterns are `orca_infill_supportbase` and
`orca_infill_concentricinternal`). They are not plugins and do not use this SDK:

- They are built in the OrcaSlicer tree, from the same object files as the application, and use a reserved ABI of
  their own, `src/libslic3r/Fill/Library/orca_infill_builtin.h`, which is not part of the SDK. Their entry point is
  `orca_infill_builtin_entry_v1`.
- A plugin must not export `orca_infill_builtin_entry_v1`, nor return the reserved status codes
  `ORCA_INFILL_ERR_STATE` and `ORCA_INFILL_ERR_CANCELED`. A file in a plugin folder that exports that entry is
  ignored, and a reserved status from a plugin is treated as `ORCA_INFILL_ERR_EXCEPTION`. A plugin cannot replace a
  built-in pattern: built-in tokens have no `.`, and the built-in libraries are only loaded from the application's own
  folder, under their own file names.
- They are tied to the build that made them. Each carries a build id (version, commit, compiler, architecture,
  configuration and a hash of the compiler flags), and OrcaSlicer only uses a library whose build id is exactly its
  own, so a library copied from another build or release is refused.
- They live next to the application: `infill_patterns` next to `orca-slicer.exe` on Windows,
  `OrcaSlicer.app/Contents/PlugIns/infill_patterns` on macOS, and `<prefix>/<libdir>/orca-slicer/infill_patterns` on
  Linux (`lib/orca-slicer/infill_patterns` inside the AppImage, `infill_patterns` next to the executable in a build
  tree).
- If one is missing or damaged, OrcaSlicer still starts and opens and saves projects, but refuses to slice and names
  the file; reinstalling fixes it. `orca-slicer --check-infill-patterns` lists every library with its status and exits
  with 1 when one is not usable.

The full contract, including the ABI versioning rules, is in the comments of `include/orca_infill_plugin.h`.
