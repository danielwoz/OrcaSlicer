/* orca_module.h - C ABI between OrcaSlicer and its modules, version 1.0.
 *
 * A module is a shared library (.dll, .so or .dylib) that exports orca_module_entry_v1(). OrcaSlicer calls it once,
 * on its main thread, before it reads any configuration, and keeps the library loaded until it exits.
 *
 * Every rule of orca_infill_plugin.h applies here too: structs start with struct_size and only grow at their end in
 * a minor version, lists of structs are arrays of pointers, strings are UTF-8, no exception or longjmp crosses the
 * boundary, everything reachable from the returned module stays valid until exit, and callbacks run concurrently,
 * must not keep pointers from a call, and their output must depend only on their arguments.
 *
 * A module can provide:
 *  - infill patterns (orca_infill_pattern); their parameters become settings "<module key>__<parameter name>";
 *  - options: settings stored in presets and projects as "<module key>__<option name>";
 *  - a fill hook, called for each layer of each object after its infill is generated;
 *  - a G-code hook, called for each layer in print order, which inserts G-code before the layer's extrusions (after
 *    the layer change and the layer's own setup, such as the temperatures set from the second layer on) and after
 *    the layer.
 * A module whose options are at their defaults must not change what OrcaSlicer prints.
 */
#ifndef ORCA_MODULE_H
#define ORCA_MODULE_H

#include "orca_infill_plugin.h"

#define ORCA_MODULE_ABI_MAJOR 1
#define ORCA_MODULE_ABI_MINOR 0
#define ORCA_MODULE_ENTRY_V1  "orca_module_entry_v1"

#ifdef __cplusplus
extern "C" {
#endif

typedef orca_infill_status orca_status; /* ORCA_INFILL_OK and the ORCA_INFILL_ERR_* codes */

/* ---- Options ---- */
#define ORCA_SCOPE_PRINT  1 /* one value per print */
/*      2 is reserved for an object scope */
#define ORCA_SCOPE_REGION 3 /* per region: print settings, then object, part, modifier and height range overrides */

/* What a change of the option recomputes; later steps follow. */
#define ORCA_STEP_GCODE          (1u << 0)
#define ORCA_STEP_INFILL         (1u << 1) /* infill paths, and the fill hook */
#define ORCA_STEP_PREPARE_INFILL (1u << 2) /* the infill areas, and everything ORCA_STEP_INFILL covers */
#define ORCA_STEP_PERIMETERS     (1u << 3)
#define ORCA_STEP_SLICE          (1u << 4)

#define ORCA_OPTION_ADVANCED           (1u << 0) /* shown in advanced mode only */
#define ORCA_OPTION_HIDE_WHEN_DISABLED (1u << 1) /* hidden, not greyed out, while enabled_if is false */

typedef struct orca_module_option {
    uint32_t                 struct_size;
    const orca_infill_param *value;       /* name, label, tooltip, unit, type, default, range and enum values */
    int32_t                  scope;       /* ORCA_SCOPE_* */
    uint32_t                 invalidates; /* ORCA_STEP_*, at least one */
    uint32_t                 flags;       /* ORCA_OPTION_* */
    const char              *enabled_if;  /* condition in the syntax of OrcaSlicer's custom G-code conditions, over the
                                             print settings and this module's options, e.g.
                                             "sparse_infill_density > 0 and infill_walls__loops > 0"; NULL: always */
} orca_module_option;

/* ---- Fill hook ---- */
typedef struct orca_layer     orca_layer;     /* one layer of one object, valid during a fill hook call */
typedef struct orca_text_sink orca_text_sink; /* G-code being inserted, valid during a G-code hook call */

/* Kinds of fill: the areas they fill and the extrusion role they print with. Bridges print with the bridge roles and
   belong to no kind, except on the first layer, where they print as bottom surfaces. */
#define ORCA_FILL_SPARSE 1 /* sparse infill areas,         role "Sparse infill" */
#define ORCA_FILL_SOLID  2 /* internal solid infill areas, role "Internal solid infill" */
#define ORCA_FILL_TOP    3 /* top surfaces,                role "Top surface" */
#define ORCA_FILL_BOTTOM 4 /* bottom surfaces,             role "Bottom surface" */
#define ORCA_FILL_MASK(kind) (1u << (kind))

typedef struct orca_fill_region {
    uint32_t      struct_size;
    const double *options;        /* every option of the module, in declaration order: this region's values for
                                     ORCA_SCOPE_REGION, the print's for ORCA_SCOPE_PRINT; ENUM as index, BOOL 0 or 1 */
    size_t        option_count;
    double        sparse_density; /* 0..1 */
    double        sparse_spacing; /* mm between the centres of adjacent lines of the sparse infill flow */
    double        solid_spacing;  /* mm, the same for the internal solid infill flow */
} orca_fill_region;

typedef struct orca_fill_layer_request {
    uint32_t                       struct_size;
    orca_layer                    *layer;
    double                         units_per_mm; /* coordinates are scaled integers in the frame of the object */
    double                         print_z;      /* mm, top of the layer */
    double                         layer_height; /* mm */
    int64_t                        layer_index;  /* counting raft layers */
    const orca_fill_region *const *regions;      /* indexed as the host services take them */
    size_t                         region_count;
} orca_fill_layer_request;

/* Receives an area to fill and the height of its extrusions in mm (more than the layer height where sparse infill
   of several layers is combined). */
typedef void (ORCA_INFILL_CALL *orca_area_visitor)(void *user, const orca_infill_region *area, double height_mm);

/* ---- G-code hook ---- */
typedef struct orca_gcode_layer_request {
    uint32_t      struct_size;
    double        print_z;          /* mm */
    double        previous_print_z; /* mm; -1 for the first layer of a pass */
    int64_t       layer_index;      /* in this pass: the whole print, or one object when printing by object */
    int64_t       layer_count;
    const double *options;          /* every option of the module; ORCA_SCOPE_REGION options hold the print settings'
                                       values, without object overrides */
    size_t        option_count;
} orca_gcode_layer_request;

/* ---- Host services: valid until exit, callable from any thread on the handles of the current call ---- */
typedef struct orca_module_host {
    uint32_t                struct_size;
    uint16_t                abi_major; /* ORCA_MODULE_ABI_MAJOR of the host */
    uint16_t                abi_minor;
    const char             *host_version;
    const orca_infill_host *infill;    /* log, clip_polylines and offset_region; emit_polyline only inside generate() */

    /* Passes each area of the given kind in a region of the layer to visit. */
    orca_status (ORCA_INFILL_CALL *visit_fill_areas)(orca_layer *layer, size_t region, int32_t kind,
                                                     orca_area_visitor visit, void *user);
    /* Removes the parts of the fills of the kinds in kind_mask (ORCA_FILL_MASK) that lie inside areas and outside keep,
       in every region of the layer, paths added earlier in this call included: add paths after the last erase. keep
       may be empty. Only paths with the kinds' roles are removed, not the gap fill between them, and ironing still
       follows the top surfaces. */
    orca_status (ORCA_INFILL_CALL *erase_fills)(orca_layer *layer, uint32_t kind_mask,
                                                const orca_infill_region *const *areas, size_t area_count,
                                                const orca_infill_region *const *keep, size_t keep_count);
    /* Adds a path of at least two points to the fills of a region, printed as kind with that kind's flow at height_mm
       (0: the layer height). A closed path returns to its first point. */
    orca_status (ORCA_INFILL_CALL *add_fill)(orca_layer *layer, size_t region, int32_t kind,
                                             const orca_infill_point *points, size_t count, int32_t closed, double height_mm);
    /* Appends text to the G-code being inserted, and a line end when the text does not end with one. The text must
       not contain a NUL byte (ORCA_INFILL_ERR_INVALID), nor comments that OrcaSlicer reads back: the G-code
       processor's tags (GCodeProcessor::reserved_tag, such as ";LAYER_CHANGE", ";TYPE:" and ";HEIGHT:", or
       "; CHANGE_LAYER" and "; FEATURE: " for Bambu Lab printers), ";Z:" and comments starting with ";_". */
    orca_status (ORCA_INFILL_CALL *append_text)(orca_text_sink *sink, const char *text, size_t length);
} orca_module_host;

/* ---- Module ---- */
typedef orca_status (ORCA_INFILL_CALL *orca_fill_layer_fn)(void *user, const orca_module_host *host,
                                                           const orca_fill_layer_request *request);
typedef orca_status (ORCA_INFILL_CALL *orca_gcode_layer_fn)(void *user, const orca_module_host *host,
                                                            const orca_gcode_layer_request *request,
                                                            orca_text_sink *before, orca_text_sink *after);

typedef struct orca_module {
    uint32_t                          struct_size;
    uint16_t                          abi_major;     /* ORCA_MODULE_ABI_MAJOR the module was built with */
    uint16_t                          abi_minor;
    const char                       *key;           /* [a-z][a-z0-9]*(_[a-z0-9]+)*, at most 24 characters; prefix of
                                                        the setting keys "<key>__<name>"; never change it */
    const char                       *label;         /* title of the module's settings group */
    const char                       *version;
    const orca_infill_pattern *const *patterns;
    size_t                            pattern_count; /* at most 64 */
    const orca_module_option  *const *options;
    size_t                            option_count;  /* at most 64 */
    void                             *user;          /* passed to the hooks; the host never reads or writes it */
    orca_fill_layer_fn                fill_layer;    /* NULL: no fill hook */
    orca_gcode_layer_fn               gcode_layer;   /* NULL: no G-code hook */
} orca_module;

/* Signature of orca_module_entry_v1(): returns the module, or NULL to decline this host. */
typedef const orca_module *(ORCA_INFILL_CALL *orca_module_entry_v1_fn)(const orca_module_host *host);

#ifdef __cplusplus
}
#endif

#endif /* ORCA_MODULE_H */
