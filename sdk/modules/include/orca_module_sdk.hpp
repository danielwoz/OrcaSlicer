#pragma once
// Helpers for writing an OrcaSlicer module in C++17. A module is a type with
//   static constexpr const char *key, *label, *version;
//   static constexpr orca_module_option options[] = { ... };                                     // optional
//   using patterns = std::tuple<Pattern...>;            // optional; patterns as for orca_infill_sdk.hpp
//   static void fill_layer(const orca_mod::FillLayer &, orca_mod::FillEdit &);                   // optional
//   static void gcode_layer(const orca_mod::GCodeLayer &, orca_mod::Text &before, orca_mod::Text &after); // optional
// and ORCA_MODULE(Module) exports the entry point. A hook throws orca_infill::Failed to report a failure.
//
// The hooks run concurrently, for different layers and prints: they must not change shared state without
// synchronisation, and their output must depend only on their arguments.
#include "orca_infill_sdk.hpp"
#include "orca_module.h"

#include <string>
#include <tuple>
#include <vector>

namespace orca_mod ORCA_INFILL_SDK_HIDDEN {

// A setting of each region, which a change of recomputes from the given steps on.
constexpr orca_module_option region_option(const orca_infill_param &value, uint32_t invalidates,
                                           const char *enabled_if = nullptr, uint32_t flags = 0)
{ return { sizeof(orca_module_option), &value, ORCA_SCOPE_REGION, invalidates, flags, enabled_if }; }
// A setting of the whole print, by default used by the G-code hook only.
constexpr orca_module_option print_option(const orca_infill_param &value, const char *enabled_if = nullptr,
                                          uint32_t flags = 0, uint32_t invalidates = ORCA_STEP_GCODE)
{ return { sizeof(orca_module_option), &value, ORCA_SCOPE_PRINT, invalidates, flags, enabled_if }; }

// An area to fill and the height of its extrusions, mm.
struct Area { orca_infill::Region region; double height; };

// One region of a layer, as a fill hook sees it.
class FillRegion
{
public:
    FillRegion(size_t index, const orca_fill_region &region) : m_index(index), m_region(&region) {}
    size_t index() const { return m_index; }
    // i: position in Module::options.
    double option(size_t i) const { return i < m_region->option_count ? m_region->options[i] : 0.; }
    double sparse_density() const { return m_region->sparse_density; }
    double sparse_spacing() const { return m_region->sparse_spacing; }
    double solid_spacing() const { return m_region->solid_spacing; }

private:
    size_t                  m_index;
    const orca_fill_region *m_region;
};

namespace detail {
inline void ORCA_INFILL_CALL collect_area(void *user, const orca_infill_region *area, double height_mm) noexcept
{
    auto &collector = *static_cast<orca_infill::detail::Collector<Area>*>(user);
    try {
        collector.out.push_back({ orca_infill::detail::copy_region(*area), height_mm });
    } catch (...) {
        collector.failed = true;
    }
}

// Views of regions as the host takes them, valid while this object and the regions live.
class RegionViews
{
public:
    explicit RegionViews(const std::vector<orca_infill::Region> &regions) : m_holes(regions.size())
    {
        m_views.reserve(regions.size());
        for (size_t i = 0; i < regions.size(); ++ i) {
            for (const orca_infill::Points &hole : regions[i].holes)
                m_holes[i].push_back({ hole.data(), hole.size() });
            m_views.push_back({ { regions[i].contour.data(), regions[i].contour.size() }, m_holes[i].data(), m_holes[i].size() });
        }
        for (const orca_infill_region &view : m_views)
            m_pointers.push_back(&view);
    }
    const orca_infill_region *const *data() const { return m_pointers.data(); }
    size_t                            size() const { return m_pointers.size(); }

private:
    std::vector<std::vector<orca_infill_path>> m_holes;
    std::vector<orca_infill_region>            m_views;
    std::vector<const orca_infill_region*>     m_pointers;
};
} // namespace detail

// One layer of one object, read-only.
class FillLayer
{
public:
    FillLayer(const orca_module_host &host, const orca_fill_layer_request &request) : m_host(host), m_request(request)
    {
        m_regions.reserve(request.region_count);
        for (size_t i = 0; i < request.region_count; ++ i)
            m_regions.emplace_back(i, *request.regions[i]);
    }
    double  print_z() const { return m_request.print_z; }
    double  layer_height() const { return m_request.layer_height; }
    int64_t layer_index() const { return m_request.layer_index; }
    int64_t units(double mm) const { return int64_t(std::llround(mm * m_request.units_per_mm)); }
    const std::vector<FillRegion>& regions() const { return m_regions; }
    // The areas of a kind of fill (ORCA_FILL_*) in a region.
    std::vector<Area> areas(const FillRegion &region, int32_t kind) const
    {
        orca_infill::detail::Collector<Area> areas;
        orca_infill::detail::check(m_host.visit_fill_areas(m_request.layer, region.index(), kind, &detail::collect_area, &areas));
        if (areas.failed)
            throw std::bad_alloc();
        return std::move(areas.out);
    }

private:
    const orca_module_host        &m_host;
    const orca_fill_layer_request &m_request;
    std::vector<FillRegion>        m_regions;
};

// Changes to the fills of a layer.
class FillEdit
{
public:
    FillEdit(const orca_module_host &host, orca_layer *layer) : m_host(host), m_layer(layer) {}
    // Removes the fills of kind_mask inside areas and outside keep, in every region of the layer, paths added earlier
    // in this call included: add paths after the last erase.
    void erase(uint32_t kind_mask, const std::vector<orca_infill::Region> &areas, const std::vector<orca_infill::Region> &keep = {})
    {
        const detail::RegionViews area_views(areas), keep_views(keep);
        orca_infill::detail::check(m_host.erase_fills(m_layer, kind_mask, area_views.data(), area_views.size(), keep_views.data(), keep_views.size()));
    }
    // Adds a path to the fills of region, printed as kind at height mm (0: the layer height).
    void add(const FillRegion &region, int32_t kind, const orca_infill::Points &points, bool closed, double height = 0.)
    { orca_infill::detail::check(m_host.add_fill(m_layer, region.index(), kind, points.data(), points.size(), closed ? 1 : 0, height)); }
    // The region grown (delta_mm > 0) or shrunk.
    std::vector<orca_infill::Region> offset(const orca_infill::Region &region, double delta_mm) const
    {
        return orca_infill::detail::with_view(region, [this, delta_mm](const orca_infill_region &view) {
            return orca_infill::detail::offset(*m_host.infill, view, delta_mm);
        });
    }
    void log(int32_t level, const char *message) const { m_host.infill->log(level, message); }

private:
    const orca_module_host &m_host;
    orca_layer             *m_layer;
};

// One layer in print order, as a G-code hook sees it.
class GCodeLayer
{
public:
    explicit GCodeLayer(const orca_gcode_layer_request &request) : m_request(request) {}
    double  print_z() const { return m_request.print_z; }
    // -1 for the first layer of a pass.
    double  previous_print_z() const { return m_request.previous_print_z; }
    int64_t layer_index() const { return m_request.layer_index; }
    int64_t layer_count() const { return m_request.layer_count; }
    // i: position in Module::options.
    double  option(size_t i) const { return i < m_request.option_count ? m_request.options[i] : 0.; }

private:
    const orca_gcode_layer_request &m_request;
};

// G-code to insert; each call adds one line, which must not contain a NUL byte or a comment OrcaSlicer reads back
// (see append_text in orca_module.h).
class Text
{
public:
    Text(const orca_module_host &host, orca_text_sink *sink) : m_host(host), m_sink(sink) {}
    void line(const std::string &text) { orca_infill::detail::check(m_host.append_text(m_sink, text.data(), text.size())); }

private:
    const orca_module_host &m_host;
    orca_text_sink         *m_sink;
};

namespace detail {
template<class M, class = void> struct has_options : std::false_type {};
template<class M> struct has_options<M, std::void_t<decltype(M::options)>> : std::true_type {};
template<class M, class = void> struct has_patterns : std::false_type {};
template<class M> struct has_patterns<M, std::void_t<typename M::patterns>> : std::true_type {};
template<class M, class = void> struct has_fill_layer : std::false_type {};
template<class M> struct has_fill_layer<M, std::void_t<decltype(&M::fill_layer)>> : std::true_type {};
template<class M, class = void> struct has_gcode_layer : std::false_type {};
template<class M> struct has_gcode_layer<M, std::void_t<decltype(&M::gcode_layer)>> : std::true_type {};

template<class M>
orca_status ORCA_INFILL_CALL fill_layer(void *, const orca_module_host *host, const orca_fill_layer_request *request) noexcept
{
    if (host == nullptr || request == nullptr || (request->region_count > 0 && request->regions == nullptr))
        return ORCA_INFILL_ERR_INVALID;
    return orca_infill::detail::guarded([&] {
        const FillLayer layer(*host, *request);
        FillEdit        edit(*host, request->layer);
        M::fill_layer(layer, edit);
    });
}

template<class M>
orca_status ORCA_INFILL_CALL gcode_layer(void *, const orca_module_host *host, const orca_gcode_layer_request *request,
                                         orca_text_sink *before, orca_text_sink *after) noexcept
{
    if (host == nullptr || request == nullptr)
        return ORCA_INFILL_ERR_INVALID;
    return orca_infill::detail::guarded([&] {
        Text before_text(*host, before), after_text(*host, after);
        M::gcode_layer(GCodeLayer(*request), before_text, after_text);
    });
}

template<class M, size_t... I>
const orca_module_option *const *option_pointers(std::index_sequence<I...>)
{ static const orca_module_option *const pointers[] = { &M::options[I]... }; return pointers; }

template<class... P>
const orca_infill_pattern *const *pattern_pointers(std::tuple<P...> *)
{ static const orca_infill_pattern *const pointers[] = { orca_infill::detail::describe<P>()... }; return pointers; }

template<class M>
const orca_module *module(const orca_module_host *host)
{
    if (host == nullptr || host->abi_major != ORCA_MODULE_ABI_MAJOR)
        return nullptr;
    static const orca_module out = [] {
        orca_module m {};
        m.struct_size = sizeof(orca_module);
        m.abi_major   = ORCA_MODULE_ABI_MAJOR;
        m.abi_minor   = ORCA_MODULE_ABI_MINOR;
        m.key         = M::key;
        m.label       = M::label;
        m.version     = M::version;
        if constexpr (has_options<M>::value) {
            constexpr size_t n = std::extent_v<decltype(M::options)>;
            m.options      = option_pointers<M>(std::make_index_sequence<n>{});
            m.option_count = n;
        }
        if constexpr (has_patterns<M>::value) {
            using Patterns = typename M::patterns;
            if constexpr (std::tuple_size_v<Patterns> > 0) {
                m.patterns      = pattern_pointers(static_cast<Patterns*>(nullptr));
                m.pattern_count = std::tuple_size_v<Patterns>;
            }
        }
        if constexpr (has_fill_layer<M>::value)
            m.fill_layer = &fill_layer<M>;
        if constexpr (has_gcode_layer<M>::value)
            m.gcode_layer = &gcode_layer<M>;
        return m;
    }();
    return &out;
}
} // namespace detail
} // namespace orca_mod

// Exports the entry point of a module.
#define ORCA_MODULE(MODULE)                                                                                           \
    extern "C" ORCA_INFILL_EXPORT const orca_module *ORCA_INFILL_CALL                                                 \
    orca_module_entry_v1(const orca_module_host *host)                                                                \
    { return ::orca_mod::detail::module<MODULE>(host); }
