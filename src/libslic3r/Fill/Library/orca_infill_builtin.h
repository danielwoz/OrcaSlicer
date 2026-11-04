/* orca_infill_builtin.h - C ABI between OrcaSlicer and the libraries that carry its built-in infill patterns,
 * version 1.0 (part of infill ABI 1.1).
 *
 * Reserved for libraries built in the OrcaSlicer source tree, in the same CMake run as libslic3r and from the same
 * object files; it is not installed with the plugin SDK. A library carries rows of the built-in pattern table and runs
 * each row's Fill class unchanged.
 *
 * Rules, in addition to those of orca_infill_plugin.h, whose geometry types, status codes, log levels, sink type and
 * ORCA_INFILL_CALL / ORCA_INFILL_EXPORT macros this header uses:
 *  - The host loads a library only from its own infill_patterns folder and only under the file name of its row,
 *    calls ORCA_INFILL_BUILTIN_ENTRY_V1 once on its main thread, binds it only when both build ids are equal byte for
 *    byte, and never unloads it. A library never adds a row and never changes row metadata.
 *  - Only the structs, pointers and scalars below cross the boundary; no C++ object, exception or allocation does.
 *    The host copies all output; a library keeps no pointer from a request, input or query after the call returns.
 *  - Enumerations travel as int32_t with the values below; the host asserts at compile time that they equal the
 *    Slic3r enumerations. Pattern values are the InfillPattern values stored in projects and profiles.
 *  - Coordinates are the host's scaled integers. scaling_factor is the host's SCALING_FACTOR (mm per unit).
 *  - run() and query_state() may run concurrently on different threads for different instances or states; an
 *    instance is never used by two threads at once. prepare_state() may run concurrently for different inputs.
 *  - A library starts no thread and uses no thread pool. prepare_state() runs parallel loops only through
 *    parallel_for / parallel_for_2d, and a loop body must not depend on how the range is split.
 *  - Every struct that starts with struct_size only gains fields at its end; a reader uses a field only when
 *    struct_size covers it and treats missing fields as zero. The value types without struct_size never change within
 *    a major version. An incompatible change raises ORCA_INFILL_BUILTIN_ABI_MAJOR and renames the entry point.
 */
#ifndef ORCA_INFILL_BUILTIN_H
#define ORCA_INFILL_BUILTIN_H

#include "orca_infill_plugin.h"

#define ORCA_INFILL_BUILTIN_ABI_MAJOR 1
#define ORCA_INFILL_BUILTIN_ABI_MINOR 0

/* ---- Enumerations ---- */
/* Slic3r::SurfaceType */
#define ORCA_INFILL_ST_TOP                            0
#define ORCA_INFILL_ST_BOTTOM                         1
#define ORCA_INFILL_ST_BOTTOM_BRIDGE                  2
#define ORCA_INFILL_ST_INTERNAL_AFTER_EXTERNAL_BRIDGE 3
#define ORCA_INFILL_ST_INTERNAL                       4
#define ORCA_INFILL_ST_INTERNAL_SOLID                 5
#define ORCA_INFILL_ST_INTERNAL_BRIDGE                6
#define ORCA_INFILL_ST_SECOND_INTERNAL_BRIDGE         7
#define ORCA_INFILL_ST_INTERNAL_VOID                  8
#define ORCA_INFILL_ST_PERIMETER                      9
/* Slic3r::ExtrusionRole */
#define ORCA_INFILL_ER_NONE                        0
#define ORCA_INFILL_ER_PERIMETER                   1
#define ORCA_INFILL_ER_EXTERNAL_PERIMETER          2
#define ORCA_INFILL_ER_OVERHANG_PERIMETER          3
#define ORCA_INFILL_ER_INTERNAL_INFILL             4
#define ORCA_INFILL_ER_SOLID_INFILL                5
#define ORCA_INFILL_ER_TOP_SOLID_INFILL            6
#define ORCA_INFILL_ER_BOTTOM_SURFACE              7
#define ORCA_INFILL_ER_IRONING                     8
#define ORCA_INFILL_ER_BRIDGE_INFILL               9
#define ORCA_INFILL_ER_INTERNAL_BRIDGE_INFILL     10
#define ORCA_INFILL_ER_GAP_FILL                   11
#define ORCA_INFILL_ER_SKIRT                      12
#define ORCA_INFILL_ER_BRIM                       13
#define ORCA_INFILL_ER_SUPPORT_MATERIAL           14
#define ORCA_INFILL_ER_SUPPORT_MATERIAL_INTERFACE 15
#define ORCA_INFILL_ER_SUPPORT_TRANSITION         16
#define ORCA_INFILL_ER_WIPE_TOWER                 17
#define ORCA_INFILL_ER_CUSTOM                     18
#define ORCA_INFILL_ER_MIXED                      19
/* Slic3r::SurfaceFillOrder */
#define ORCA_INFILL_ORDER_DEFAULT 0
#define ORCA_INFILL_ORDER_OUTWARD 1
#define ORCA_INFILL_ORDER_INWARD  2
/* Slic3r::CenterOfSurfacePattern */
#define ORCA_INFILL_CENTER_EACH_SURFACE  0
#define ORCA_INFILL_CENTER_EACH_MODEL    1
#define ORCA_INFILL_CENTER_EACH_ASSEMBLY 2
/* Slic3r::AdaptiveInfillMode */
#define ORCA_INFILL_ADAPTIVE_DISABLED       0
#define ORCA_INFILL_ADAPTIVE_DISTANCE_WARP  1
#define ORCA_INFILL_ADAPTIVE_SMOOTH_BLEND   2
#define ORCA_INFILL_ADAPTIVE_STEPPED_SHELLS 3
#define ORCA_INFILL_ADAPTIVE_LOBES          4
#define ORCA_INFILL_ADAPTIVE_NORMAL_Z       5
#define ORCA_INFILL_ADAPTIVE_NORMAL_Y       6
#define ORCA_INFILL_ADAPTIVE_NORMAL_X       7
/* Slic3r::AdaptiveInfillGradient */
#define ORCA_INFILL_GRADIENT_LINEAR      0
#define ORCA_INFILL_GRADIENT_QUADRATIC   1
#define ORCA_INFILL_GRADIENT_EXPONENTIAL 2

/* ---- Calls of run(), traits, object state ---- */
#define ORCA_INFILL_CALL_FILL_SURFACE        1 /* Fill::fill_surface(): open polylines */
#define ORCA_INFILL_CALL_FILL_ARACHNE        2 /* Fill::fill_surface_arachne(): thick polylines */
#define ORCA_INFILL_CALL_LOCKED_ZAG          3 /* FillLockedZag::fill_surface_locked_zag(): polylines grouped by flow */
#define ORCA_INFILL_CALL_CONCENTRIC_INTERNAL 4 /* FillConcentricInternal::concentric_internal_lines(): thick polylines */

#define ORCA_INFILL_TRAIT_NO_SORT (1u << 0)    /* Fill::no_sort(); must equal the row's ipfNoSort */

#define ORCA_INFILL_STATE_OCTREE            1  /* FillAdaptive::Octree */
#define ORCA_INFILL_STATE_LIGHTNING         2  /* FillLightning::Generator of the infill */
#define ORCA_INFILL_STATE_LIGHTNING_SUPPORT 3  /* FillLightning::Generator of tree support */
#define ORCA_INFILL_STATE_ADAPTIVE_FIELD    4  /* AdaptiveInfillField */
#define ORCA_INFILL_QUERY_LIGHTNING_LINES   1  /* getTreesForLayer(layer).convertToLines(area, overlap) */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Value types (no struct_size; fixed within a major version) ---- */
typedef struct orca_infill_flow     { float width, height, spacing, nozzle_diameter; uint8_t bridge; } orca_infill_flow; /* Flow */
typedef struct orca_infill_regions  { const orca_infill_region *items; size_t count; } orca_infill_regions;            /* ExPolygons */
typedef struct orca_infill_polygons { const orca_infill_path   *items; size_t count; } orca_infill_polygons;           /* Polygons */
typedef struct orca_infill_thick_path {                       /* ThickPolyline */
    const orca_infill_point *points;  size_t count;
    const double            *widths;  size_t width_count;     /* ThickPolyline::width, scaled, 2 * (count - 1) */
    uint8_t                  first_endpoint, last_endpoint;   /* ThickPolyline::endpoints */
} orca_infill_thick_path;
typedef struct orca_infill_density_regions { float density; orca_infill_regions regions; } orca_infill_density_regions;
typedef struct orca_infill_flow_regions    { orca_infill_flow flow; orca_infill_regions regions; } orca_infill_flow_regions;

/* ---- Mirrors of the C++ state of one filler call ---- */
typedef struct orca_infill_surface {          /* Slic3r::Surface */
    uint32_t           struct_size;
    int32_t            surface_type;          /* ORCA_INFILL_ST_* */
    orca_infill_region expolygon;             /* verbatim: no reorientation, no point dropped or repeated */
    double             thickness;             /* mm; -1 undefined */
    double             bridge_angle;          /* radians; < 0 undefined */
    uint16_t           thickness_layers;      /* raw, not clamped */
    uint16_t           extra_perimeters;
} orca_infill_surface;

typedef struct orca_infill_fill_params {      /* Slic3r::FillParams: every field except config, in declaration order */
    uint32_t         struct_size;
    float            density;
    int32_t          multiline;
    float            anchor_length, anchor_length_max;
    double           resolution;
    uint8_t          dont_adjust, monotonic, complete, use_arachne;
    double           layer_height;
    uint8_t          gyroid_optimized;
    int32_t          adaptive_infill_mode;               /* ORCA_INFILL_ADAPTIVE_* */
    float            adaptive_infill_interior_density;
    int32_t          adaptive_infill_gradient;           /* ORCA_INFILL_GRADIENT_* */
    double           smooth_factor;
    double           lateral_lattice_angle_1, lateral_lattice_angle_2;
    int32_t          pattern;                            /* InfillPattern exactly as the caller set it */
    float            infill_overhang_angle;
    orca_infill_flow flow;
    int32_t          extrusion_role;                     /* ORCA_INFILL_ER_* */
    uint8_t          using_internal_flow;
    float            no_extrusion_overlap;
    uint8_t          dont_sort, can_reverse;
    int32_t          fill_order;                         /* ORCA_INFILL_ORDER_* */
    float            horiz_move;
    uint8_t          symmetric_infill_y_axis, infill_complete_top;
    int64_t          symmetric_y_axis;                   /* scaled */
    uint8_t          locked_zag;
    float            infill_lock_depth, skin_infill_depth;
    int32_t          center_of_surface_pattern;          /* ORCA_INFILL_CENTER_* */
} orca_infill_fill_params;

typedef struct orca_infill_fill_state {       /* the members of Slic3r::Fill */
    uint32_t            struct_size;
    uint64_t            layer_id;             /* size_t; UINT64_MAX is size_t(-1) */
    double              z, spacing, overlap;  /* mm */
    float               angle;                /* radians; FLT_MAX undefined */
    uint8_t             fixed_angle, dont_alternate_fill_direction;
    uint8_t             bounding_box_defined; /* BoundingBox::defined; min and max are passed verbatim either way */
    uint8_t             calib_flowrate_topinfill_special_order; /* replaces the print_object_config read */
    int64_t             link_max_length, loop_clipping;          /* scaled */
    orca_infill_bbox    bounding_box;
    double              min_nozzle_diameter;  /* mm; replaces the print_config read; 0 without a printer config */
    void               *state;                /* from this library's prepare_state(), or NULL */
    orca_infill_regions no_overlap;           /* Fill::no_overlap_expolygons; CALL_CONCENTRIC_INTERNAL only */
} orca_infill_fill_state;

typedef struct orca_infill_lock {             /* Slic3r::LockRegionParam, each std::map in its iteration order */
    uint32_t                           struct_size;
    const orca_infill_density_regions *skin_density;     size_t skin_density_count;
    const orca_infill_density_regions *skeleton_density; size_t skeleton_density_count;
    const orca_infill_flow_regions    *skeleton_flow;    size_t skeleton_flow_count;
    const orca_infill_flow_regions    *skin_flow;        size_t skin_flow_count;
} orca_infill_lock;

typedef struct orca_infill_builtin_request {
    uint32_t                       struct_size;
    int32_t                        call;            /* ORCA_INFILL_CALL_* */
    double                         scaling_factor;  /* the host's SCALING_FACTOR */
    const orca_infill_surface     *surface;
    const orca_infill_fill_params *params;
    const orca_infill_fill_state  *fill;
    const orca_infill_lock        *lock;            /* after Fill::set_lock_region_param(); NULL otherwise */
} orca_infill_builtin_request;

/* ---- Object state ---- */
typedef struct orca_infill_state_input { uint32_t struct_size; int32_t kind; const void *data; } orca_infill_state_input;

typedef struct orca_infill_octree_input {     /* FillAdaptive::build_octree(), kind OCTREE */
    uint32_t       struct_size;
    const float   *vertices;  size_t vertex_count;          /* x, y, z per vertex, octree frame */
    const int32_t *indices;   size_t triangle_count;        /* 3 per triangle */
    const double  *overhangs; size_t overhang_vertex_count; /* x, y, z, 3 vertices per triangle, octree frame */
    double         line_spacing;
    uint8_t        support_overhangs_only;
} orca_infill_octree_input;

typedef struct orca_infill_lightning_input {  /* FillLightning::Generator, kinds LIGHTNING and LIGHTNING_SUPPORT */
    uint32_t                    struct_size;
    size_t                      layer_count;
    const orca_infill_polygons *outlines;     /* per layer. LIGHTNING: stInternal and stInternalVoid fill surfaces of every
                                                 region, region then surface order. LIGHTNING_SUPPORT: the contours */
    const orca_infill_polygons *overhangs;    /* LIGHTNING_SUPPORT: per layer; NULL otherwise */
    float                       infill_extrusion_width;  /* scaled */
    int64_t                     supporting_radius, wall_supporting_radius, prune_length, straightening_max_distance;
} orca_infill_lightning_input;

typedef struct orca_infill_field_slice { double bottom_z, print_z; orca_infill_regions lslices; } orca_infill_field_slice;
typedef struct orca_infill_field_input {      /* AdaptiveInfillField, kind ADAPTIVE_FIELD */
    uint32_t                       struct_size;
    const orca_infill_field_slice *slices; size_t slice_count;
    orca_infill_bbox               bbox;
    uint8_t                        bbox_defined;
    int32_t                        mode;      /* ORCA_INFILL_ADAPTIVE_* */
} orca_infill_field_input;

typedef struct orca_infill_state_query {
    uint32_t             struct_size;
    int32_t              kind;                /* ORCA_INFILL_QUERY_* */
    uint64_t             layer;
    orca_infill_polygons area;
    int64_t              overlap;             /* scaled */
} orca_infill_state_query;

typedef struct orca_infill_job orca_infill_job;  /* host context of one prepare_state() call */
typedef void (ORCA_INFILL_CALL *orca_infill_range_fn)(void *user, int64_t begin, int64_t end);
typedef void (ORCA_INFILL_CALL *orca_infill_range2d_fn)(void *user, int64_t row_begin, int64_t row_end,
                                                       int64_t col_begin, int64_t col_end);

/* ---- Host table ---- */
typedef struct orca_infill_builtin_host {
    uint32_t    struct_size;
    uint16_t    abi_major, abi_minor;         /* ORCA_INFILL_BUILTIN_ABI_* of the host */
    const char *build_id;                     /* ORCA_INFILL_BUILD_ID of the host */
    /* Output of run() and query_state(); copied by the host and kept verbatim and in order. */
    orca_infill_status (ORCA_INFILL_CALL *emit_polyline)(orca_infill_sink *sink, const orca_infill_point *points,
                                                         size_t count);                 /* any count, also 0 or 1 */
    orca_infill_status (ORCA_INFILL_CALL *emit_thick_polyline)(orca_infill_sink *sink, const orca_infill_thick_path *path);
    /* CALL_LOCKED_ZAG: the following polylines use flow flow_index: lock->skeleton_flow[i] for i < skeleton_flow_count,
       then lock->skin_flow[i - skeleton_flow_count]. */
    orca_infill_status (ORCA_INFILL_CALL *begin_group)(orca_infill_sink *sink, size_t flow_index);
    void               (ORCA_INFILL_CALL *set_spacing)(orca_infill_sink *sink, double spacing_mm); /* also with ERR_FAILED */
    void               (ORCA_INFILL_CALL *set_error)(orca_infill_sink *sink, const char *message);
    /* Logging, from any thread. */
    int32_t            (ORCA_INFILL_CALL *log_enabled)(int32_t level);
    void               (ORCA_INFILL_CALL *log)(int32_t level, const char *message);
    /* prepare_state() only, from any thread that takes part in the job. */
    int32_t            (ORCA_INFILL_CALL *is_canceled)(orca_infill_job *job);
    orca_infill_status (ORCA_INFILL_CALL *parallel_for)(orca_infill_job *job, int64_t begin, int64_t end, int64_t grain,
                                                        orca_infill_range_fn body, void *user);
    orca_infill_status (ORCA_INFILL_CALL *parallel_for_2d)(orca_infill_job *job, int64_t row_begin, int64_t row_end,
                                                           int64_t row_grain, int64_t col_begin, int64_t col_end,
                                                           int64_t col_grain, orca_infill_range2d_fn body, void *user);
} orca_infill_builtin_host;

/* ---- Library ---- */
typedef struct orca_infill_builtin_row {
    uint32_t    struct_size;
    int32_t     pattern;                      /* InfillPattern value of the row */
    const char *key;                          /* the row's token; NULL for an internal row */
    uint32_t    calls;                        /* (1u << ORCA_INFILL_CALL_*) for each call run() accepts */
    uint32_t    traits;                       /* ORCA_INFILL_TRAIT_* */
    uint32_t    state_kinds;                  /* (1u << ORCA_INFILL_STATE_*) for each kind prepare_state() builds */
} orca_infill_builtin_row;

typedef struct orca_infill_builtin_library {
    uint32_t                              struct_size;
    uint16_t                              abi_major, abi_minor;   /* ORCA_INFILL_BUILTIN_ABI_* it was built with */
    const char                           *build_id;
    const char                           *name;                   /* file stem, e.g. "orca_infill_gyroid" */
    const orca_infill_builtin_row *const *rows;
    size_t                                row_count;              /* 1 for every library OrcaSlicer ships */
    /* Main thread, while no call runs: at binding, and whenever the host changes SCALING_FACTOR. */
    void               (ORCA_INFILL_CALL *set_scaling_factor)(double scaling_factor);
    /* One instance per host Fill object; create() returns NULL when it runs out of memory. */
    void              *(ORCA_INFILL_CALL *create)(int32_t pattern);
    void               (ORCA_INFILL_CALL *destroy)(void *instance);
    orca_infill_status (ORCA_INFILL_CALL *run)(void *instance, const orca_infill_builtin_request *request,
                                               orca_infill_sink *sink);
    /* Object state; NULL when no row has state_kinds. *state = NULL: nothing to build (e.g. an empty field). */
    orca_infill_status (ORCA_INFILL_CALL *prepare_state)(int32_t pattern, const orca_infill_state_input *input,
                                                         orca_infill_job *job, void **state);
    void               (ORCA_INFILL_CALL *release_state)(void *state);
    orca_infill_status (ORCA_INFILL_CALL *query_state)(void *state, const orca_infill_state_query *query,
                                                       orca_infill_sink *sink);
} orca_infill_builtin_library;

/* Exported as ORCA_INFILL_BUILTIN_ENTRY_V1. Returns NULL, after host->log, when host->abi_major or host->build_id is
   not the library's own. */
typedef const orca_infill_builtin_library *(ORCA_INFILL_CALL *orca_infill_builtin_entry_v1_fn)(const orca_infill_builtin_host *host);

#ifdef __cplusplus
}
#endif
#endif /* ORCA_INFILL_BUILTIN_H */
