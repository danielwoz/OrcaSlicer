#include "ModuleHooks.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include <orca_module.h>

#include "libslic3r_version.h"
#include "../ClipperUtils.hpp"
#include "../Exception.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../I18N.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../format.hpp"
#include "../Fill/FillPlugin.hpp"
#include "ModuleRegistry.hpp"

namespace Slic3r {
namespace {

constexpr uint32_t all_kinds = ORCA_FILL_MASK(ORCA_FILL_SPARSE) | ORCA_FILL_MASK(ORCA_FILL_SOLID) | ORCA_FILL_MASK(ORCA_FILL_TOP) |
                               ORCA_FILL_MASK(ORCA_FILL_BOTTOM);

// The orca_layer of a fill hook call: a layer of an object. The mutex serialises the services that change the fills,
// which a module may call from several threads.
struct ModuleLayer
{
    Layer     &layer;
    std::mutex mutex;
};

ModuleLayer& module_layer(orca_layer *layer) { return *reinterpret_cast<ModuleLayer*>(layer); }

bool valid_kind(int32_t kind) { return kind >= ORCA_FILL_SPARSE && kind <= ORCA_FILL_BOTTOM; }

// Whether a surface of a layer is an area of a kind of fill, one make_fills() prints with the kind's role. Bridges
// print with bridge roles, except bottom bridges on the first layer.
bool is_area_of(int32_t kind, SurfaceType type, const Layer &layer)
{
    switch (kind) {
    case ORCA_FILL_SPARSE: return type == stInternal;
    case ORCA_FILL_SOLID:  return type == stInternalSolid;
    case ORCA_FILL_TOP:    return type == stTop;
    default:               return type == stBottom || (type == stBottomBridge && layer.id() == 0);
    }
}

// The extrusion role a kind of fill prints with.
ExtrusionRole role_of(int32_t kind)
{
    switch (kind) {
    case ORCA_FILL_SPARSE: return erInternalInfill;
    case ORCA_FILL_SOLID:  return erSolidInfill;
    case ORCA_FILL_TOP:    return erTopSolidInfill;
    default:               return erBottomSurface;
    }
}

bool valid_regions(const orca_infill_region *const *regions, size_t count)
{
    if (count > 0 && regions == nullptr)
        return false;
    return std::all_of(regions, regions + count, [](const orca_infill_region *region) { return valid_infill_region(region); });
}

ExPolygons to_expolygons(const orca_infill_region *const *regions, size_t count)
{
    ExPolygons out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++ i)
        out.emplace_back(infill_region_expolygon(*regions[i]));
    return out;
}

BoundingBox extents(const Polyline3 &polyline)
{
    BoundingBox out;
    for (const Point3 &p : polyline.points)
        out.merge(Point(p.x(), p.y()));
    return out;
}

// Removes the parts of the paths of some roles that lie inside an area.
class FillEraser
{
public:
    FillEraser(uint32_t kind_mask, ExPolygons area) : m_area(std::move(area)), m_bbox(get_extents(m_area))
    {
        for (int32_t kind = ORCA_FILL_SPARSE; kind <= ORCA_FILL_BOTTOM; ++ kind)
            if (kind_mask & ORCA_FILL_MASK(kind))
                m_roles.push_back(role_of(kind));
    }

    // Erases from a collection and the collections in it, and deletes the collections left empty. A path the area cuts
    // is replaced by copies along its remaining pieces, so they keep every attribute; a multi-path or loop the area cuts
    // is replaced by its paths. Nothing is deleted before the collection holds its new entities, so a failure leaves
    // the layer valid.
    void erase(ExtrusionEntityCollection &collection) const
    {
        ExtrusionEntitiesPtr kept, removed;
        kept.reserve(collection.entities.size());
        removed.reserve(collection.entities.size());
        for (ExtrusionEntity *entity : collection.entities) {
            auto *child = dynamic_cast<ExtrusionEntityCollection*>(entity);
            if (child != nullptr)
                this->erase(*child);
            if (child != nullptr ? child->entities.empty() : this->cut(*entity, kept))
                removed.push_back(entity);
            else
                kept.push_back(entity);
        }
        collection.entities = std::move(kept);
        for (ExtrusionEntity *entity : removed)
            delete entity;
    }

private:
    // The pieces of a path outside the area, in the order and direction of the path; nullopt when the area does not cut it.
    std::optional<Polylines> pieces(const ExtrusionPath &path) const
    {
        if (std::find(m_roles.begin(), m_roles.end(), path.role()) == m_roles.end() || ! m_bbox.overlap(extents(path.polyline)))
            return std::nullopt;
        const Polyline source = path.polyline.to_polyline();
        Polylines      out    = diff_pl(Polylines{ source }, m_area);
        if (out.size() == 1 && out.front().length() > source.length() - SCALED_EPSILON)
            return std::nullopt;
        out.erase(std::remove_if(out.begin(), out.end(), [](const Polyline &piece) { return piece.size() < 2; }), out.end());
        restore_source_path_order(source, out);
        return out;
    }

    // Appends a copy of path to out, along piece when it is given.
    static void append_copy(const ExtrusionPath &path, const Polyline *piece, bool fixed_direction, ExtrusionEntitiesPtr &out)
    {
        std::unique_ptr<ExtrusionPath> copy(static_cast<ExtrusionPath*>(path.clone()));
        if (piece != nullptr)
            copy->polyline = Polyline3(*piece);
        if (fixed_direction)
            copy->set_reverse();
        out.push_back(copy.get());
        copy.release();
    }

    // Appends what remains of entity to out and returns true; returns false, appending nothing, when the area does not
    // cut any of its paths.
    bool cut(const ExtrusionEntity &entity, ExtrusionEntitiesPtr &out) const
    {
        const bool fixed_direction = ! entity.can_reverse();
        if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity)) {
            const std::optional<Polylines> remaining = this->pieces(*path);
            if (! remaining)
                return false;
            for (const Polyline &piece : *remaining)
                append_copy(*path, &piece, fixed_direction, out);
            return true;
        }
        const ExtrusionPaths *paths = nullptr;
        if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(&entity))
            paths = &multi->paths;
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity))
            paths = &loop->paths;
        if (paths == nullptr)
            return false;
        std::vector<std::optional<Polylines>> remaining;
        remaining.reserve(paths->size());
        for (const ExtrusionPath &path : *paths)
            remaining.emplace_back(this->pieces(path));
        if (std::none_of(remaining.begin(), remaining.end(), [](const std::optional<Polylines> &r) { return r.has_value(); }))
            return false;
        for (size_t i = 0; i < paths->size(); ++ i)
            if (! remaining[i])
                append_copy((*paths)[i], nullptr, fixed_direction, out);
            else
                for (const Polyline &piece : *remaining[i])
                    append_copy((*paths)[i], &piece, fixed_direction, out);
        return true;
    }

    ExPolygons                 m_area;
    BoundingBox                m_bbox;
    std::vector<ExtrusionRole> m_roles;
};

// ---- Host services: callable from any thread on the handles of the current call, never throw ----

orca_status ORCA_INFILL_CALL host_visit_fill_areas(orca_layer *handle, size_t region, int32_t kind, orca_area_visitor visit,
                                                   void *user) noexcept
{
    if (handle == nullptr || ! valid_kind(kind) || visit == nullptr || region >= module_layer(handle).layer.region_count())
        return ORCA_INFILL_ERR_INVALID;
    return host_service([&] {
        const Layer &layer = module_layer(handle).layer;
        for (const Surface &surface : layer.get_region(int(region))->fill_surfaces.surfaces)
            if (is_area_of(kind, surface.surface_type, layer)) {
                const InfillPluginRegion area(surface.expolygon);
                visit(user, &area.view(), surface.thickness == -1 ? layer.height : surface.thickness);
            }
    });
}

orca_status ORCA_INFILL_CALL host_erase_fills(orca_layer *handle, uint32_t kind_mask, const orca_infill_region *const *areas,
                                              size_t area_count, const orca_infill_region *const *keep, size_t keep_count) noexcept
{
    if (handle == nullptr || kind_mask == 0 || (kind_mask & ~all_kinds) != 0 || ! valid_regions(areas, area_count) ||
        ! valid_regions(keep, keep_count))
        return ORCA_INFILL_ERR_INVALID;
    return host_service([&] {
        ExPolygons area = diff_ex(union_ex(to_expolygons(areas, area_count)), union_ex(to_expolygons(keep, keep_count)));
        if (area.empty())
            return;
        const FillEraser            eraser(kind_mask, std::move(area));
        ModuleLayer                &target = module_layer(handle);
        std::lock_guard<std::mutex> lock(target.mutex);
        for (LayerRegion *layerm : target.layer.regions())
            eraser.erase(layerm->fills);
    });
}

orca_status ORCA_INFILL_CALL host_add_fill(orca_layer *handle, size_t region, int32_t kind, const orca_infill_point *points, size_t count,
                                           int32_t closed, double height_mm) noexcept
{
    if (handle == nullptr || ! valid_kind(kind) || points == nullptr || count < 2 || ! std::isfinite(height_mm) || height_mm < 0. ||
        region >= module_layer(handle).layer.region_count())
        return ORCA_INFILL_ERR_INVALID;
    return host_service([&] {
        ModuleLayer &target = module_layer(handle);
        LayerRegion &layerm = *target.layer.get_region(int(region));
        const Flow   flow   = layerm.flow(kind == ORCA_FILL_SPARSE ? frInfill : kind == ORCA_FILL_TOP ? frTopSolidInfill : frSolidInfill,
                                          height_mm > 0. ? height_mm : target.layer.height);
        auto path = std::make_unique<ExtrusionPath>(role_of(kind), flow.mm3_per_mm(), flow.width(), flow.height());
        Points3 &out = path->polyline.points;
        out.reserve(count + 1);
        for (size_t i = 0; i < count; ++ i)
            out.emplace_back(coord_t(points[i].x), coord_t(points[i].y), coord_t(0));
        if (closed != 0 && out.front() != out.back())
            out.push_back(out.front());
        // A collection of its own, as make_fills() gives each surface: G-code assigns each fills collection to the
        // island holding its first point, and the tool ordering picks one filament per collection from its role.
        auto collection = std::make_unique<ExtrusionEntityCollection>();
        collection->entities.push_back(path.get());
        path.release();
        std::lock_guard<std::mutex> lock(target.mutex);
        layerm.fills.entities.push_back(collection.get());
        collection.release();
    });
}

orca_status ORCA_INFILL_CALL host_append_text(orca_text_sink *sink, const char *text, size_t length) noexcept
{
    // A NUL would end the G-code written to the file there.
    if (sink == nullptr || (text == nullptr && length > 0) || (length > 0 && std::memchr(text, 0, length) != nullptr))
        return ORCA_INFILL_ERR_INVALID;
    return host_service([&] {
        std::string &out = *reinterpret_cast<std::string*>(sink);
        if (length > 0)
            out.append(text, length);
        if (length == 0 || text[length - 1] != '\n')
            out += '\n';
    });
}

orca_text_sink* text_sink(std::string &text) { return reinterpret_cast<orca_text_sink*>(&text); }

// Calls a hook of a module with its log prefix set.
template<class F> orca_status call_hook(const ModuleRecord &module, F &&call)
{
    return call_guarded("Module " + module.key + ": ", std::forward<F>(call));
}

// A hook that fails ends the slice or the export: the host cannot undo what it already changed.
[[noreturn]] void throw_hook_failure(const ModuleRecord &module, double print_z, orca_status status)
{
    throw SlicingError(Slic3r::format(_u8L("The module \"%1%\" failed on the layer at %2% mm (error %3%)."), module.label, print_z, status));
}

const orca_module& abi_of(const ModuleRecord &module) { return *static_cast<const orca_module*>(module.abi); }

} // namespace

const orca_module_host& module_host()
{
    static const orca_module_host host { sizeof(orca_module_host), ORCA_MODULE_ABI_MAJOR, ORCA_MODULE_ABI_MINOR, SoftFever_VERSION,
                                         &infill_plugin_host(), &host_visit_fill_areas, &host_erase_fills, &host_add_fill,
                                         &host_append_text };
    return host;
}

void run_module_fill_hooks(Layer &layer)
{
    const PrintConfig &print_config = layer.object()->print()->config();
    const size_t       count        = layer.region_count();
    for (const ModuleRecord *module : ModuleRegistry::modules()) {
        if (! module->has_fill_hook)
            continue;
        std::vector<std::vector<double>>     values(count);
        std::vector<orca_fill_region>        regions(count);
        std::vector<const orca_fill_region*> pointers(count);
        for (size_t i = 0; i < count; ++ i) {
            const LayerRegion       &layerm = *layer.get_region(int(i));
            const PrintRegionConfig &config = layerm.region().config();
            values[i]   = ModuleRegistry::option_values(*module, print_config, config);
            regions[i]  = { sizeof(orca_fill_region), values[i].data(), values[i].size(), config.sparse_infill_density.value / 100.,
                            layerm.flow(frInfill).spacing(), layerm.flow(frSolidInfill).spacing() };
            pointers[i] = &regions[i];
        }
        ModuleLayer                   handle { layer, {} };
        const orca_fill_layer_request request { sizeof(orca_fill_layer_request), reinterpret_cast<orca_layer*>(&handle), 1. / SCALING_FACTOR,
                                                layer.print_z, layer.height, int64_t(layer.id()), pointers.data(), count };
        const orca_module &abi    = abi_of(*module);
        const orca_status  status = call_hook(*module, [&] { return abi.fill_layer(abi.user, &module_host(), &request); });
        if (status != ORCA_INFILL_OK)
            throw_hook_failure(*module, layer.print_z, status);
    }
}

std::vector<ModuleGCodeHook> module_gcode_hooks(const Print &print)
{
    std::vector<ModuleGCodeHook> out;
    for (const ModuleRecord *module : ModuleRegistry::modules())
        if (module->has_gcode_hook)
            out.push_back({ module, ModuleRegistry::option_values(*module, print.config(), print.default_region_config()) });
    return out;
}

void apply_module_gcode_hooks(const std::vector<ModuleGCodeHook> &hooks, std::string &gcode, size_t setup_end, double print_z,
                              double previous_print_z, size_t layer_index, size_t layer_count)
{
    std::string before, after;
    for (const ModuleGCodeHook &hook : hooks) {
        const orca_gcode_layer_request request { sizeof(orca_gcode_layer_request), print_z, previous_print_z, int64_t(layer_index),
                                                 int64_t(layer_count), hook.options.data(), hook.options.size() };
        const orca_module &abi = abi_of(*hook.module);
        std::string        hook_before, hook_after;
        const orca_status  status = call_hook(*hook.module, [&] {
            return abi.gcode_layer(abi.user, &module_host(), &request, text_sink(hook_before), text_sink(hook_after));
        });
        if (status != ORCA_INFILL_OK)
            throw_hook_failure(*hook.module, print_z, status);
        before += hook_before;
        after += hook_after;
    }
    if (! before.empty())
        gcode.insert(std::min(setup_end, gcode.size()), before);
    gcode += after;
}

} // namespace Slic3r
