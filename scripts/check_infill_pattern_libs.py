#!/usr/bin/env python3
"""Audits a folder of built-in infill pattern libraries (src/infill_patterns).

    check_infill_pattern_libs.py --manifest <file> [--exact] [--dumpbin <path>] [--config <name>] <folder>

The manifest names one library file per line, as src/infill_patterns/CMakeLists.txt
writes it. For the folder and each library it checks that:

- the folder holds every library of the manifest and no other pattern library
  (orca_infill_*); with --exact, no other file at all;
- a library exports orca_infill_builtin_entry_v1 and nothing else;
- it imports only the C and C++ runtimes, and on Linux has no library search path;
- it carries none of the code a library must not contain: the TBB runtime or
  allocator, Boost.Log, a thread, GMP or MPFR, or code that reads print
  configurations, presets or the print, or that belongs to wxWidgets, OCCT or
  OpenVDB;
- on macOS, it holds a slice for every architecture of the first library.

--config names the configuration the libraries were built in; a Windows Debug
build also imports what DEBUG_IMPORTS lists.
Linux reads the libraries with readelf and nm, macOS with nm, otool and lipo,
Windows with dumpbin and the map its link writes next to each library. Exits
with 1 and one line per problem when a check fails, and with 77 when a tool it
needs is not available.
"""

import argparse
import os
import re
import subprocess
import sys

ENTRY = "orca_infill_builtin_entry_v1"
# The exit code of a run that could not read the libraries, the SKIP_RETURN_CODE of its test.
SKIPPED = 77

# Demangled symbols a library must neither define nor reference.
FORBIDDEN = [
    (r"tbb::detail::r1::", "the TBB runtime"),
    (r"\brml::|__TBB_malloc_|\bscalable_(realloc|calloc|posix_memalign|aligned_|msize|allocation_)", "the TBB allocator"),
    (r"boost::log::", "Boost.Log"),
    (r"\bpthread_create\b|\bCreateThread\b|\b_beginthreadex\b|std::thread::_M_start_thread", "a thread"),
    (r"Slic3r::PrintConfigDef\b|Slic3r::ConfigBase::option\b|Slic3r::Preset\b|Slic3r::AppConfig\b|"
     r"Slic3r::PlaceholderParser\b|Slic3r::Print::|Slic3r::PrintObject::", "application code"),
    (r"\bwx[A-Z]\w*", "wxWidgets"),
    (r"\b(Standard_Transient|TopoDS_\w+|BRep_\w+|gp_[A-Z]\w*)\b", "OCCT"),
    (r"\bopenvdb::", "OpenVDB"),
]

# Code of GMP or MPFR, which are DLLs on Windows. Of the shared sources only the exact planarity check of
# Geometry/VoronoiUtilsCgal.cpp uses them, and no pattern calls it, so a link that drops the functions a library does not
# reach leaves none of that code.
EXACT_ARITHMETIC = (r"__gmp[a-z]*_\w|(?:^|_)mpfr_\w", "GMP or MPFR")

# The C and C++ runtimes. On Linux that includes the dynamic loader, which defines __tls_get_addr for the thread-local
# variables of the shared sources.
IMPORTS = {
    "linux": [r"libstdc\+\+\.so\.\d+", r"libm\.so\.\d+", r"libgcc_s\.so\.\d+", r"libc\.so\.\d+", r"ld-linux[\w.-]*\.so\.\d+"],
    "darwin": [r"/usr/lib/libc\+\+\.1\.dylib", r"/usr/lib/libSystem\.B\.dylib"],
    "win32": [r"KERNEL32\.dll", r"VCRUNTIME140(_1)?\.dll", r"MSVCP140(_\d+)?\.dll", r"api-ms-win-crt-[\w-]+\.dll"],
}

# The further imports of the libraries of a Windows Debug build: the debug C and C++ runtimes, and GMP and MPFR
# (EXACT_ARITHMETIC). That build compiles without function-level linking, so a library keeps all of an object it uses:
# the exact planarity check of Geometry/VoronoiUtilsCgal.cpp comes with the angle check that validates the Voronoi
# diagrams of Arachne.
DEBUG_IMPORTS = {
    "win32": [r"VCRUNTIME140(_1)?D\.dll", r"MSVCP140(_\d+)?D\.dll", r"ucrtbased\.dll", r"libgmp-10\.dll", r"libmpfr-4\.dll"],
}


# Not an OSError, so that it is not taken for a library that cannot be read.
class ToolMissing(Exception):
    pass


def tool(command, **options):
    try:
        return subprocess.run(command, capture_output=True, text=True, **options)
    except FileNotFoundError:
        raise ToolMissing(command[0])


def run(*command):
    result = tool(command)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(command)}: {result.stderr.strip()}")
    return result.stdout


def demangle(names):
    if not names:
        return []
    result = tool(["c++filt"], input="\n".join(names))
    if result.returncode != 0:
        raise RuntimeError(f"c++filt: {result.stderr.strip()}")
    return result.stdout.splitlines()


def linux(path):
    exports, imports, symbols = set(), [], []
    for line in run("readelf", "--dyn-syms", "--wide", path).splitlines():
        fields = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(fields) >= 8 and fields[0].rstrip(":").isdigit():
            name = fields[7].split("@")[0]
            if fields[6] != "UND" and fields[4] in ("GLOBAL", "WEAK", "GNU_UNIQUE") and fields[3] != "SECTION":
                exports.add(name)
            symbols.append(name)
    paths = []
    for line in run("readelf", "--dynamic", "--wide", path).splitlines():
        match = re.search(r"\(NEEDED\)\s+Shared library: \[(.+)\]", line)
        if match:
            imports.append(match.group(1))
        match = re.search(r"\((RPATH|RUNPATH)\)\s+Library r(un)?path: \[(.*)\]", line)
        if match:
            paths.append(match.group(3))
    # The full symbol table of an unstripped library, every symbol it defines or references.
    nm = tool(["nm", "--no-sort", path])
    if nm.returncode == 0:
        symbols += [line.split()[-1] for line in nm.stdout.splitlines() if line.split()]
    return exports, imports, demangle(symbols), [f"searches {path}" for path in paths]


def darwin(path):
    exports = {line.split()[-1] for line in run("nm", "-gU", path).splitlines() if line.split()}
    exports = {name[1:] if name.startswith("_") else name for name in exports}
    imports = [line.split()[0] for line in run("otool", "-L", path).splitlines()[1:] if line.strip()]
    symbols = [line.split()[-1] for line in run("nm", path).splitlines() if line.split()]
    symbols = [name[1:] if name.startswith("_") else name for name in symbols]
    return exports, imports, demangle(symbols), []


def msvc_map_symbols(path):
    """The symbols of the map the link of a Windows library wrote next to it, and the scopes of their decorated names
    written as C++ names: ?get@core@log@boost@@YAXXZ also gives boost::log::core::get."""
    map_path = os.path.splitext(path)[0] + ".map"
    if not os.path.isfile(map_path):
        return []
    with open(map_path, encoding="utf-8", errors="replace") as file:
        # <section>:<offset>  <decorated name>  <address> [f] <library>:<object>
        symbols = [match.group(1) for match in (re.match(r"^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)", line) for line in file)
                   if match]
    return symbols + ["::".join(reversed(scope.split("@")[:-2]))
                      for symbol in symbols for scope in re.findall(r"(?:[A-Za-z_]\w*@)+@", symbol)]


def win32(path, dumpbin):
    exports = set()
    in_table = False
    for line in run(dumpbin, "/nologo", "/exports", path).splitlines():
        fields = line.split()
        if fields[:4] == ["ordinal", "hint", "RVA", "name"]:
            in_table = True
        elif in_table and len(fields) >= 4 and fields[0].isdigit():
            exports.add(fields[3])
        elif in_table and fields and fields[0] == "Summary":
            in_table = False
    imports = [line.strip() for line in run(dumpbin, "/nologo", "/dependents", path).splitlines()
               if line.strip().lower().endswith(".dll") and "Dump of file" not in line]
    # The imported functions, through which a thread would be created, and the code the library holds.
    symbols = [line.split()[-1] for line in run(dumpbin, "/nologo", "/imports", path).splitlines()
               if re.match(r"^\s+[0-9A-Fa-f]+\s+\S+$", line)]
    return exports, imports, symbols + msvc_map_symbols(path), []


def main():
    parser = argparse.ArgumentParser(description="Audits a folder of built-in infill pattern libraries.")
    parser.add_argument("--manifest", required=True, help="file with one library file name per line")
    parser.add_argument("--exact", action="store_true", help="the folder holds nothing but the libraries")
    parser.add_argument("--dumpbin", default="dumpbin", help="dumpbin of Visual Studio, on Windows")
    parser.add_argument("--config", default="", help="the build configuration of the libraries")
    parser.add_argument("folder")
    args = parser.parse_args()

    with open(args.manifest, encoding="utf-8") as file:
        expected = [line.strip() for line in file if line.strip()]
    platform = "linux" if sys.platform.startswith("linux") else sys.platform
    if platform not in IMPORTS:
        print(f"check_infill_pattern_libs: unsupported platform {sys.platform}")
        return 1
    read = {"linux": linux, "darwin": darwin, "win32": lambda path: win32(path, args.dumpbin)}[platform]
    allowed, forbidden = IMPORTS[platform], FORBIDDEN + [EXACT_ARITHMETIC]
    if args.config == "Debug" and platform in DEBUG_IMPORTS:
        allowed, forbidden = allowed + DEBUG_IMPORTS[platform], FORBIDDEN

    problems = []
    present = sorted(os.listdir(args.folder)) if os.path.isdir(args.folder) else []
    if not os.path.isdir(args.folder):
        problems.append(f"{args.folder}: no such folder")
    for name in expected:
        if name not in present:
            problems.append(f"{name}: missing")
    suffix = os.path.splitext(expected[0])[1] if expected else None
    for name in present:
        library = suffix is not None and name.startswith("orca_infill_") and name.endswith(suffix)
        if name not in expected and (args.exact or library):
            problems.append(f"{name}: not in the manifest")

    archs = None
    for name in expected:
        path = os.path.join(args.folder, name)
        if not os.path.isfile(path):
            continue
        try:
            exports, imports, symbols, notes = read(path)
        except (OSError, RuntimeError) as error:
            problems.append(f"{name}: cannot be read: {error}")
            continue
        problems += [f"{name}: {note}" for note in notes]
        if exports != {ENTRY}:
            problems.append(f"{name}: exports {sorted(exports)}, not only {ENTRY}")
        for library in imports:
            if not any(re.fullmatch(pattern, os.path.basename(library) if platform == "linux" else library, re.IGNORECASE)
                       for pattern in allowed):
                problems.append(f"{name}: imports {library}")
        for pattern, what in forbidden:
            hits = sorted({symbol for symbol in symbols if re.search(pattern, symbol)})
            if hits:
                problems.append(f"{name}: carries {what}: {', '.join(hits[:5])}{' ...' if len(hits) > 5 else ''}")
        if platform == "darwin":
            slices = set(run("lipo", "-archs", path).split())
            if archs is None:
                archs = slices
            elif slices != archs:
                problems.append(f"{name}: architectures {sorted(slices)}, not {sorted(archs)}")

    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print(f"OK {len(expected)} infill pattern libraries in {args.folder}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ToolMissing as missing:
        print(f"SKIP {missing} is not available")
        sys.exit(SKIPPED)
