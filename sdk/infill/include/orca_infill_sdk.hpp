#pragma once
// Helpers for writing an OrcaSlicer infill plugin in C++17. A pattern is a type with
//   static constexpr const char *key, *label;  static constexpr uint32_t slots, caps, post;
//   static constexpr orca_infill_param params[] = { ... };   // optional
//   static void generate(const orca_infill::Request &, orca_infill::Output &);
// and ORCA_INFILL_PLUGIN("name", "version", Pattern...) exports the entry point.
//
// generate() runs concurrently on several threads, for different layers and regions: it must not change shared state
// without synchronisation, must not start thread pools, and its output must depend only on the request.
#include "orca_infill_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

// The SDK stays private to each plugin whatever its build flags, so two plugins never share its statics, as GCC's
// unique symbols would make them do even across libraries loaded with RTLD_LOCAL.
#if defined(__GNUC__) && ! defined(_WIN32) && ! defined(__CYGWIN__)
#  define ORCA_INFILL_SDK_HIDDEN __attribute__((visibility("hidden")))
#else
#  define ORCA_INFILL_SDK_HIDDEN
#endif

namespace orca_infill ORCA_INFILL_SDK_HIDDEN {

using Point  = orca_infill_point;
using Points = std::vector<Point>;
struct Region { Points contour; std::vector<Points> holes; };

// Thrown by generate() when the region cannot be filled; the host leaves it empty.
struct Failed {};

constexpr orca_infill_param float_param(const char *name, const char *label, double def, double min, double max,
                                        const char *unit = nullptr, const char *tooltip = nullptr)
{ return { sizeof(orca_infill_param), name, label, tooltip, unit, ORCA_INFILL_PARAM_FLOAT, def, min, max, nullptr, nullptr, 0 }; }
constexpr orca_infill_param int_param(const char *name, const char *label, int def, int min, int max,
                                      const char *unit = nullptr, const char *tooltip = nullptr)
{ return { sizeof(orca_infill_param), name, label, tooltip, unit, ORCA_INFILL_PARAM_INT, double(def), double(min), double(max), nullptr, nullptr, 0 }; }
constexpr orca_infill_param bool_param(const char *name, const char *label, bool def, const char *tooltip = nullptr)
{ return { sizeof(orca_infill_param), name, label, tooltip, nullptr, ORCA_INFILL_PARAM_BOOL, def ? 1. : 0., 0., 1., nullptr, nullptr, 0 }; }
template<size_t N>
constexpr orca_infill_param enum_param(const char *name, const char *label, const char *const (&values)[N],
                                       const char *const (&labels)[N], size_t def, const char *tooltip = nullptr)
{ return { sizeof(orca_infill_param), name, label, tooltip, nullptr, ORCA_INFILL_PARAM_ENUM, double(def), 0., double(N - 1), values, labels, N }; }

// Read-only view of a request.
class Request
{
public:
    explicit Request(const orca_infill_request &r) : m_r(r) {}
    const orca_infill_region &region() const { return *m_r.region; }
    const orca_infill_bbox   &bbox() const { return m_r.bbox; }
    Point   origin() const { return m_r.origin; }
    double  spacing() const { return m_r.spacing; }
    double  density() const { return m_r.density; }
    double  z() const { return m_r.z; }
    double  layer_height() const { return m_r.layer_height; }
    int64_t layer_index() const { return m_r.layer_index; }
    int     multiline() const { return m_r.multiline; }
    int     role() const { return m_r.role; }
    double  param(size_t i) const { return i < m_r.param_count ? m_r.params[i] : 0.; }
    // Millimetres between pattern lines for the requested density, leaving room for the host's multiline copies.
    double  line_distance() const { return m_r.spacing * m_r.multiline / std::max(m_r.density, 1e-3); }
    int64_t units(double mm) const { return int64_t(std::llround(mm * m_r.units_per_mm)); }
    double  mm(int64_t units) const { return double(units) / m_r.units_per_mm; }
    // The smallest value >= v on the grid through origin with the given step.
    static int64_t align_up(int64_t v, int64_t origin, int64_t step)
    { const int64_t d = v - origin, q = d / step - ((d % step) < 0 ? 1 : 0); const int64_t a = origin + q * step; return a < v ? a + step : a; }
    const orca_infill_request &raw() const { return m_r; }
private:
    const orca_infill_request &m_r;
};

namespace detail {
// Throws std::bad_alloc for ORCA_INFILL_ERR_NOMEM and Failed for any other error.
inline void check(orca_infill_status status)
{
    if (status == ORCA_INFILL_ERR_NOMEM)
        throw std::bad_alloc();
    if (status != ORCA_INFILL_OK)
        throw Failed();
}

// Collects what a host service visits. Visitors are called by the host, so they never throw.
template<class T> struct Collector { std::vector<T> out; bool failed { false }; };

inline Points copy_points(const orca_infill_point *points, size_t count) { return Points(points, points + count); }

inline void ORCA_INFILL_CALL collect_path(void *user, const orca_infill_point *points, size_t count) noexcept
{
    auto &collector = *static_cast<Collector<Points>*>(user);
    try {
        collector.out.emplace_back(copy_points(points, count));
    } catch (...) {
        collector.failed = true;
    }
}

inline void ORCA_INFILL_CALL collect_region(void *user, const orca_infill_region *region) noexcept
{
    auto &collector = *static_cast<Collector<Region>*>(user);
    try {
        Region out { copy_points(region->contour.points, region->contour.count), {} };
        for (size_t i = 0; i < region->hole_count; ++ i)
            out.holes.emplace_back(copy_points(region->holes[i].points, region->holes[i].count));
        collector.out.emplace_back(std::move(out));
    } catch (...) {
        collector.failed = true;
    }
}

// Calls f with a view of region, valid during the call.
template<class F> auto with_view(const Region &region, F &&f)
{
    std::vector<orca_infill_path> holes;
    holes.reserve(region.holes.size());
    for (const Points &hole : region.holes)
        holes.push_back({ hole.data(), hole.size() });
    const orca_infill_region view { { region.contour.data(), region.contour.size() }, holes.data(), holes.size() };
    return f(view);
}
} // namespace detail

// Output of a request, and the host services.
class Output
{
public:
    Output(const orca_infill_host &host, orca_infill_sink *sink) : m_host(host), m_sink(sink) {}
    // Throws std::bad_alloc when the host runs out of memory.
    void polyline(const Point *points, size_t count)
    { if (m_host.emit_polyline(m_sink, points, count) == ORCA_INFILL_ERR_NOMEM) throw std::bad_alloc(); }
    void polyline(const Points &points) { this->polyline(points.data(), points.size()); }
    void line(Point a, Point b) { const Point pts[2] { a, b }; this->polyline(pts, 2); }
    // Pieces of the polylines inside region.
    std::vector<Points> clip(const std::vector<Points> &polylines, const orca_infill_region &region) const
    {
        std::vector<orca_infill_path> paths;
        paths.reserve(polylines.size());
        for (const Points &polyline : polylines)
            paths.push_back({ polyline.data(), polyline.size() });
        detail::Collector<Points> pieces;
        detail::check(m_host.clip_polylines(paths.data(), paths.size(), &region, &detail::collect_path, &pieces));
        if (pieces.failed)
            throw std::bad_alloc();
        return std::move(pieces.out);
    }
    // The region grown (delta_mm > 0) or shrunk.
    std::vector<Region> offset(const orca_infill_region &region, double delta_mm) const
    {
        detail::Collector<Region> regions;
        detail::check(m_host.offset_region(&region, delta_mm, &detail::collect_region, &regions));
        if (regions.failed)
            throw std::bad_alloc();
        return std::move(regions.out);
    }
    // The same for a region that offset() returned.
    std::vector<Points> clip(const std::vector<Points> &polylines, const Region &region) const
    { return detail::with_view(region, [&](const orca_infill_region &view) { return this->clip(polylines, view); }); }
    std::vector<Region> offset(const Region &region, double delta_mm) const
    { return detail::with_view(region, [&](const orca_infill_region &view) { return this->offset(view, delta_mm); }); }
    void log(int32_t level, const char *message) const { m_host.log(level, message); }
private:
    const orca_infill_host &m_host;
    orca_infill_sink       *m_sink;
};

namespace detail {
template<class P, class = void> struct has_params : std::false_type {};
template<class P> struct has_params<P, std::void_t<decltype(P::params)>> : std::true_type {};

// The C entry of a pattern: maps exceptions to status codes.
template<class P>
orca_infill_status ORCA_INFILL_CALL generate(void *, const orca_infill_host *host, const orca_infill_request *request,
                                             orca_infill_sink *sink) noexcept
{
    if (host == nullptr || request == nullptr || sink == nullptr || request->region == nullptr)
        return ORCA_INFILL_ERR_INVALID;
    try {
        Output out(*host, sink);
        P::generate(Request(*request), out);
        return ORCA_INFILL_OK;
    } catch (const Failed &) {
        return ORCA_INFILL_ERR_FAILED;
    } catch (const std::bad_alloc &) {
        return ORCA_INFILL_ERR_NOMEM;
    } catch (...) {
        return ORCA_INFILL_ERR_EXCEPTION;
    }
}

template<class P, size_t... I>
const orca_infill_param *const *param_pointers(std::index_sequence<I...>)
{ static const orca_infill_param *const pointers[] = { &P::params[I]... }; return pointers; }

template<class P>
const orca_infill_pattern *describe()
{
    static const orca_infill_pattern pattern = [] {
        orca_infill_pattern p {};
        p.struct_size = sizeof(orca_infill_pattern);
        p.key = P::key; p.label = P::label; p.slots = P::slots; p.caps = P::caps; p.post = P::post;
        if constexpr (has_params<P>::value) {
            constexpr size_t n = std::extent_v<decltype(P::params)>;
            p.params      = param_pointers<P>(std::make_index_sequence<n>{});
            p.param_count = n;
        }
        p.generate = &generate<P>;
        return p;
    }();
    return &pattern;
}

template<class... P>
const orca_infill_plugin *plugin(const orca_infill_host *host, const char *name, const char *version)
{
    if (host == nullptr || host->abi_major != ORCA_INFILL_ABI_MAJOR)
        return nullptr;
    static const orca_infill_pattern *const patterns[] = { describe<P>()... };
    static const orca_infill_plugin out { sizeof(orca_infill_plugin), ORCA_INFILL_ABI_MAJOR, ORCA_INFILL_ABI_MINOR,
                                          name, version, patterns, sizeof...(P) };
    return &out;
}
} // namespace detail
} // namespace orca_infill

// Exports the entry point of a plugin made of the given pattern types.
#define ORCA_INFILL_PLUGIN(NAME, VERSION, ...)                                                                        \
    extern "C" ORCA_INFILL_EXPORT const orca_infill_plugin *ORCA_INFILL_CALL                                          \
    orca_infill_plugin_entry_v1(const orca_infill_host *host)                                                         \
    { return ::orca_infill::detail::plugin<__VA_ARGS__>(host, NAME, VERSION); }
