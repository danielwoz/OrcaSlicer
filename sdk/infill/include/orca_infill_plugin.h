/* orca_infill_plugin.h - C ABI between OrcaSlicer and infill pattern plugins, version 1.1.
 *
 * A plugin is a shared library (.dll, .so or .dylib) that exports orca_infill_plugin_entry_v1(). OrcaSlicer calls
 * it once, on its main thread, before it reads any configuration, and keeps the library loaded until it exits.
 *
 * Rules
 *  - Only the plain structs, pointers and scalars below cross the boundary. Each side frees only what it allocated;
 *    the host copies everything a plugin emits.
 *  - Strings are UTF-8 and NUL-terminated. Everything reachable from the struct the entry point returns stays valid
 *    and unchanged until the process exits.
 *  - No C++ exception or longjmp may leave a function on either side.
 *  - generate() runs concurrently on several threads, for different layers and regions. It must not change shared
 *    state without synchronisation, must not start thread pools and must not keep pointers from a request after it
 *    returns. Its output must depend only on the request.
 *  - Every struct except the geometry types (point, bbox, path and region) starts with struct_size. A minor version
 *    only appends fields at the end of such a struct; a reader uses a field only when struct_size covers it and treats
 *    missing fields as zero. Lists of them are arrays of pointers, so a larger struct never changes an array stride.
 *    The geometry types are passed by value and in plain arrays, so they never change within a major version.
 *    An incompatible change raises ORCA_INFILL_ABI_MAJOR and renames the entry point.
 *  - Coordinates are scaled integers; request->units_per_mm of them make a millimetre. The scale depends on the size
 *    of the print bed, so it is never a constant.
 *  - Version 1.1 adds no fields. It reserves an entry point and two status codes for the libraries that carry the
 *    built-in patterns (orca_infill_builtin.h, in the OrcaSlicer source tree). A plugin never exports
 *    ORCA_INFILL_BUILTIN_ENTRY_V1 and never returns a reserved status: the host does not load a plugin file that
 *    exports it, and treats a reserved status returned by a plugin as ORCA_INFILL_ERR_EXCEPTION.
 */
#ifndef ORCA_INFILL_PLUGIN_H
#define ORCA_INFILL_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#define ORCA_INFILL_ABI_MAJOR 1
#define ORCA_INFILL_ABI_MINOR 1
#define ORCA_INFILL_ENTRY_V1  "orca_infill_plugin_entry_v1"
#define ORCA_INFILL_BUILTIN_ENTRY_V1 "orca_infill_builtin_entry_v1" /* reserved: built-in pattern libraries only */

#if defined(_WIN32)
#  define ORCA_INFILL_CALL   __cdecl
#  define ORCA_INFILL_EXPORT __declspec(dllexport)
#else
#  define ORCA_INFILL_CALL
#  define ORCA_INFILL_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t orca_infill_status;
#define ORCA_INFILL_OK              0
#define ORCA_INFILL_ERR_FAILED    (-1) /* this region cannot be filled; the host leaves it empty */
#define ORCA_INFILL_ERR_INVALID   (-2) /* invalid arguments */
#define ORCA_INFILL_ERR_NOMEM     (-3)
#define ORCA_INFILL_ERR_EXCEPTION (-4) /* an exception was caught inside the plugin */
#define ORCA_INFILL_ERR_STATE     (-5) /* reserved: a built-in library's scaling factor is not the request's */
#define ORCA_INFILL_ERR_CANCELED  (-6) /* reserved: the host asked a built-in library's prepare_state() to stop */

typedef struct orca_infill_point { int64_t x, y; } orca_infill_point;
typedef struct orca_infill_bbox  { orca_infill_point min, max; } orca_infill_bbox;
/* A polyline, or, as a contour or a hole, a polygon whose first point is not repeated at its end. */
typedef struct orca_infill_path  { const orca_infill_point *points; size_t count; } orca_infill_path;
/* An area: a counter-clockwise contour with clockwise holes. */
typedef struct orca_infill_region { orca_infill_path contour; const orca_infill_path *holes; size_t hole_count; } orca_infill_region;

/* ---- Parameters: declared by the plugin, values chosen by the user ---- */
#define ORCA_INFILL_PARAM_FLOAT 1
#define ORCA_INFILL_PARAM_INT   2
#define ORCA_INFILL_PARAM_BOOL  3
#define ORCA_INFILL_PARAM_ENUM  4

typedef struct orca_infill_param {
    uint32_t           struct_size;
    const char        *name;          /* [a-z0-9_]{1,32}, unique within the pattern; stored in projects */
    const char        *label;         /* shown to the user */
    const char        *tooltip;       /* may be NULL */
    const char        *unit;          /* text after the value, such as "mm"; may be NULL */
    int32_t            type;          /* ORCA_INFILL_PARAM_* */
    double             default_value; /* BOOL: 0 or 1; ENUM: index into enum_values */
    double             min_value;     /* FLOAT and INT */
    double             max_value;
    const char *const *enum_values;   /* ENUM: stored tokens, [a-z0-9_-]{1,32} */
    const char *const *enum_labels;   /* ENUM: shown to the user */
    size_t             enum_count;    /* ENUM: 1 to 64 */
} orca_infill_param;

/* ---- Request: one call per region of a layer; every pointer is valid during the call only ---- */
#define ORCA_INFILL_ROLE_SPARSE         0
#define ORCA_INFILL_ROLE_INTERNAL_SOLID 1
#define ORCA_INFILL_ROLE_TOP            2
#define ORCA_INFILL_ROLE_BOTTOM         3

typedef struct orca_infill_request {
    uint32_t                  struct_size;
    const orca_infill_region *region;           /* area to fill, in the pattern frame */
    orca_infill_bbox          bbox;             /* bounds of region grown by spacing * multiline, pattern frame */
    orca_infill_point         origin;           /* pattern frame: centre of the object, or of the connected body with
                                                   separated infills; lay grids out from it so the pattern continues
                                                   across regions and layers */
    double                    units_per_mm;
    double                    angle;            /* radians; the pattern frame is the print frame rotated by -angle about
                                                   (0,0); includes infill_direction and the layer alternation */
    double                    spacing;          /* mm between the centres of adjacent solid lines of this flow */
    double                    density;          /* 0 < density <= 1 */
    double                    z;                /* mm, top of the layer */
    double                    layer_height;     /* mm */
    int64_t                   layer_index;      /* layer number counting raft layers; -1 when unknown */
    uint32_t                  thickness_layers; /* layers combined into this one by infill_combination, >= 1 */
    int32_t                   multiline;        /* lines per infill wall: fill_multiline with CAP_MULTILINE, whose copies
                                                   the host draws; 1 without it */
    int32_t                   role;             /* ORCA_INFILL_ROLE_* */
    const double             *params;           /* one value per declared parameter, in declaration order; ENUM as index */
    size_t                    param_count;
} orca_infill_request;

/* ---- Host services: a table owned by the host, valid until the process exits, callable from any thread ---- */
typedef struct orca_infill_sink orca_infill_sink; /* host-owned output of one generate() call */
typedef void (ORCA_INFILL_CALL *orca_infill_path_visitor)(void *user, const orca_infill_point *points, size_t count);
typedef void (ORCA_INFILL_CALL *orca_infill_region_visitor)(void *user, const orca_infill_region *region);

#define ORCA_INFILL_LOG_DEBUG   0
#define ORCA_INFILL_LOG_INFO    1
#define ORCA_INFILL_LOG_WARNING 2
#define ORCA_INFILL_LOG_ERROR   3

typedef struct orca_infill_host {
    uint32_t    struct_size;
    uint16_t    abi_major;
    uint16_t    abi_minor;
    const char *host_version;  /* e.g. "2.4.0" */
    /* Appends an open polyline of at least two points, in the pattern frame, to the output of the current call. */
    orca_infill_status (ORCA_INFILL_CALL *emit_polyline)(orca_infill_sink *sink, const orca_infill_point *points, size_t count);
    /* Writes to the application log, naming the plugin. */
    void               (ORCA_INFILL_CALL *log)(int32_t level, const char *message);
    /* Clips polylines to a region; each piece inside is passed to visit and is valid during that call only. */
    orca_infill_status (ORCA_INFILL_CALL *clip_polylines)(const orca_infill_path *polylines, size_t count,
                                                          const orca_infill_region *region,
                                                          orca_infill_path_visitor visit, void *user);
    /* Grows (delta_mm > 0) or shrinks a region; each resulting region is passed to visit, valid during that call only. */
    orca_infill_status (ORCA_INFILL_CALL *offset_region)(const orca_infill_region *region, double delta_mm,
                                                         orca_infill_region_visitor visit, void *user);
} orca_infill_host;

/* ---- Patterns ---- */
#define ORCA_INFILL_SLOT_SPARSE  (1u << 0) /* sparse_infill_pattern */
#define ORCA_INFILL_SLOT_SURFACE (1u << 1) /* top_surface_pattern, bottom_surface_pattern, internal_solid_infill_pattern */

#define ORCA_INFILL_CAP_MULTILINE            (1u << 0) /* fill_multiline applies; the host draws the copies */
#define ORCA_INFILL_CAP_SEPARABLE            (1u << 1) /* separated_infills applies: the origin moves to each body */
#define ORCA_INFILL_CAP_FIXED_ANGLE          (1u << 2) /* no 90 degree turn between layers */
#define ORCA_INFILL_CAP_KEEP_ORDER           (1u << 3) /* print the paths in the order emitted */
#define ORCA_INFILL_CAP_ROTATE_TEMPLATE_SAFE (1u << 4) /* keeps its strength under sparse_infill_rotate_template */

/* Host processing of the emitted lines, in this order: SMOOTH, the multiline copies of CAP_MULTILINE, CLIP,
   DROP_SHORT, CONNECT; then the lines are rotated back into the print frame. */
#define ORCA_INFILL_POST_SMOOTH     (1u << 0) /* round corners by sparse_infill_smooth_factor */
#define ORCA_INFILL_POST_CLIP       (1u << 1) /* keep the parts inside the region; without it the lines must lie inside */
#define ORCA_INFILL_POST_DROP_SHORT (1u << 2) /* drop pieces shorter than 0.8 x spacing */
#define ORCA_INFILL_POST_CONNECT    (1u << 3) /* join line ends along the outline as infill_anchor allows; needs CLIP */
#define ORCA_INFILL_POST_DEFAULT    (ORCA_INFILL_POST_CLIP | ORCA_INFILL_POST_DROP_SHORT | ORCA_INFILL_POST_CONNECT)

/* Emits the lines of one region through host->emit_polyline(sink, ...). */
typedef orca_infill_status (ORCA_INFILL_CALL *orca_infill_generate_fn)(void *user, const orca_infill_host *host,
                                                                       const orca_infill_request *request,
                                                                       orca_infill_sink *sink);

typedef struct orca_infill_pattern {
    uint32_t                        struct_size;
    const char                     *key;   /* the token stored in configurations: two or more parts matching
                                              [a-z0-9][a-z0-9_-]* joined by '.', at most 64 characters,
                                              e.g. "org.example.waves" */
    const char                     *label; /* shown in the pattern lists */
    uint32_t                        slots; /* ORCA_INFILL_SLOT_* */
    uint32_t                        caps;  /* ORCA_INFILL_CAP_* */
    uint32_t                        post;  /* ORCA_INFILL_POST_* */
    const orca_infill_param *const *params;
    size_t                          param_count; /* at most 32 */
    void                           *user;  /* passed to generate(); the host never reads or writes it */
    orca_infill_generate_fn         generate;
} orca_infill_pattern;

typedef struct orca_infill_plugin {
    uint32_t                          struct_size;
    uint16_t                          abi_major; /* ORCA_INFILL_ABI_MAJOR the plugin was built with */
    uint16_t                          abi_minor;
    const char                       *name;
    const char                       *version;
    const orca_infill_pattern *const *patterns;
    size_t                            pattern_count; /* at most 64 */
} orca_infill_plugin;

/* Signature of orca_infill_plugin_entry_v1(): returns the plugin, or NULL to decline this host. */
typedef const orca_infill_plugin *(ORCA_INFILL_CALL *orca_infill_plugin_entry_v1_fn)(const orca_infill_host *host);

#ifdef __cplusplus
}
#endif

#endif /* ORCA_INFILL_PLUGIN_H */
