# Built-in infill pattern libraries

Every row of the built-in pattern table (`src/libslic3r/Fill/InfillPatternRegistry.cpp`) that has a filler is served
by a loadable library of its own, `orca_infill_<token>`. The library runs the row's `Fill` class unchanged: it links
the same object files as everything else, and only plain C structs cross the boundary
(`src/libslic3r/Fill/Library/orca_infill_builtin.h`). The application binds a library only when its build id equals
its own, and refuses to slice while any library of the build is missing or damaged.

- `<family>.cmake`: the rows of one family, one `orca_add_builtin_infill_library()` call each. `CMakeLists.txt`
  includes the families in a fixed order and generates the manifest, `orca_infill_libraries.h` and the build tree
  copies.
- `runtime/`: the entry template (`PatternEntry.hpp`, `entry.cpp.in`), the definitions a library has in place of
  libslic3r's (`ModuleSeams.cpp`: log, allocator, parallel loops), the table of the entries linked into an executable
  (`LinkedLibraries.cpp`) and the export lists.
- The filler sources stay in `src/libslic3r/Fill/`. `libslic3r_add_pattern_objects()` in
  `src/libslic3r/CMakeLists.txt` compiles them once, into `orca_fill_*_obj` object libraries; the code they share with
  libslic3r is in `libslic3r_fillkit`.

## Adding a built-in pattern library

1. Add the pattern as any built-in pattern: its `InfillPattern` value after the existing ones, its row in
   `s_builtins` with `builtin_fill<ORCA_INFILL_LIBRARY_<ROW>>()` as its factory
   (`optional_fill<ORCA_INFILL_<NAME>, ORCA_INFILL_LIBRARY_<ROW>>()` for one a build may leave out), and its settings.
   `<ROW>` is the token in upper case without punctuation; add it to `ORCA_INFILL_ROWS` in `CMakeLists.txt`, in
   `InfillPattern` order. `<NAME>` of an optional pattern is its name in `ORCA_OPTIONAL_INFILL_PATTERNS`
   (`src/libslic3r/CMakeLists.txt`), which defines the `ORCA_INFILL_<NAME>` option of the build.
2. Compile its sources once: `libslic3r_add_pattern_objects(orca_fill_<name>_obj Fill/FillX.cpp Fill/FillX.hpp)`.
3. Add its library to a family file, or a new one listed in `CMakeLists.txt`:
   ```cmake
   orca_add_builtin_infill_library(<token> ROW ipX CLASS FillX HEADER libslic3r/Fill/FillX.hpp
                                   OBJECTS orca_fill_<name>_obj [CALLS ...] [STATE ...] [NO_SORT] [INTERNAL])
   ```
   `CALLS`, `STATE` and `NO_SORT` must say what the loader expects from the row's flags (`contract()` in
   `Fill/InfillPatternLibraries.cpp`): `FILL_ARACHNE` for `ipfArachne`, `NO_SORT` for `ipfNoSort`, `OCTREE` for
   `ipfOctree`, `ADAPTIVE_FIELD` for `ipfAdaptiveDensity`. Otherwise the library is rejected at start-up. The call of
   an optional pattern goes in `if (ORCA_INFILL_<NAME>) ... endif ()`, so that a build without the pattern has no
   library for it either (`optional_fill` asserts that the two agree).
4. A library links with `--no-undefined` on Linux. If it needs a libslic3r source that is not in the kit, add that
   source to `libslic3r_fillkit` in `src/libslic3r/CMakeLists.txt`, provided it does not reach configuration, print or
   GUI code.
5. Tests: add the row to the corpus (`tests/infill_patterns/corpus.cpp`) and to `native_filler()`
   (`tests/infill_patterns/native_fillers.cpp`), then regenerate the golden digests from a tree that slices correctly
   with `ORCA_INFILL_GOLDEN_WRITE=<path>`. The equivalence test then compares the library with the class, and the
   `infill_pattern_libraries_audit` test checks its exports, imports and symbols.

## Linking the libraries in

`-DORCA_INFILL_PATTERN_LINKAGE=STATIC` builds no library files: libslic3r links the entries and the pattern objects,
and the loader binds them from the executable (`--check-infill-patterns` then says "libraries linked into the
executable"). The objects and the C path are the same as with the default `MODULE`, so the G-code is too; it suits
sanitizer and debugging builds. In a `MODULE` build, tools that ship as a single file, such as the profile validator,
link the entries with `orca_link_builtin_infill_patterns()`, and targets that run the `Fill` classes directly, such as
the tests, link the objects with `orca_link_infill_pattern_objects()`.
