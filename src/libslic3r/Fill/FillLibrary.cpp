#include "FillLibrary.hpp"

#include <iterator>
#include <new>
#include <utility>

#include "../Exception.hpp"
#include "../I18N.hpp"
#include "../Surface.hpp"
#include "../format.hpp"
#include "InfillObjectState.hpp"
#include "InfillPatternLibraries.hpp"
#include "InfillPatternRegistry.hpp"
#include "Library/BuiltinMirror.hpp"
#include "Library/orca_infill_builtin.h"

namespace Slic3r {

Fill* FillLibrary::create(const InfillPatternDescriptor &pattern)
{
    const InfillPatternBinding *binding = InfillPatternRegistry::binding(pattern.id);
    if (binding == nullptr || binding->library == nullptr)
        throw SlicingError(builtin_infill_library_error(pattern.id));
    return new FillLibrary(pattern.id, *binding->library, binding->file);
}

FillLibrary::FillLibrary(InfillPattern pattern, const orca_infill_builtin_library &library, std::string file)
    : m_pattern(pattern), m_library(&library), m_file(std::move(file)), m_instance(library.create(int32_t(pattern)))
{
    if (m_instance == nullptr)
        throw std::bad_alloc();
}

FillLibrary::FillLibrary(const FillLibrary &other)
    : Fill(other)
    , m_pattern(other.m_pattern)
    , m_library(other.m_library)
    , m_file(other.m_file)
    , m_instance(other.m_library->create(int32_t(other.m_pattern)))
    , m_lock(other.m_lock ? std::make_unique<LockRegionParam>(*other.m_lock) : nullptr)
{
    if (m_instance == nullptr)
        throw std::bad_alloc();
}

FillLibrary::~FillLibrary()
{
    if (! builtin_infill_patterns_shut_down())
        m_library->destroy(m_instance);
}

bool FillLibrary::no_sort() const { return infill_pattern(m_pattern).has(ipfNoSort); }

Polylines FillLibrary::fill_surface(const Surface *surface, const FillParams &params)
{
    BuiltinInfillOutput out;
    this->check(this->run(ORCA_INFILL_CALL_FILL_SURFACE, *surface, params, out), out);
    return std::move(out.polylines);
}

ThickPolylines FillLibrary::fill_surface_arachne(const Surface *surface, const FillParams &params)
{
    BuiltinInfillOutput out;
    this->check(this->run(ORCA_INFILL_CALL_FILL_ARACHNE, *surface, params, out), out);
    return std::move(out.thick_polylines);
}

void FillLibrary::set_lock_region_param(const LockRegionParam &lock_param) { m_lock = std::make_unique<LockRegionParam>(lock_param); }

void FillLibrary::fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out)
{
    if (m_pattern == ipConcentricInternal) {
        BuiltinInfillOutput lines;
        this->check(this->run(ORCA_INFILL_CALL_CONCENTRIC_INTERNAL, *surface, params, lines), lines);
        this->append_concentric_internal_extrusions(lines.thick_polylines, params, out);
        return;
    }
    if (m_pattern != ipLockedZag) {
        Fill::fill_surface_extrusion(surface, params, out);
        return;
    }
    std::vector<std::pair<Polylines, Flow>> multi_width_polyline;
    try {
        this->fill_surface_locked_zag(surface, params, multi_width_polyline);
    }
    catch (InfillFailedException&) {}
    this->append_locked_zag_extrusions(std::move(multi_width_polyline), params, out);
}

void FillLibrary::fill_surface_locked_zag(const Surface *surface, const FillParams &params,
                                          std::vector<std::pair<Polylines, Flow>> &multi_width_polyline)
{
    BuiltinInfillOutput      out;
    const orca_infill_status status = this->run(ORCA_INFILL_CALL_LOCKED_ZAG, *surface, params, out);
    if (status == ORCA_INFILL_OK || status == ORCA_INFILL_ERR_FAILED)
        for (std::pair<size_t, Polylines> &group : out.groups)
            multi_width_polyline.emplace_back(std::move(group.second), this->lock_flow(group.first));
    this->check(status, out);
}

const Flow& FillLibrary::lock_flow(size_t flow_index) const
{
    if (m_lock) {
        if (flow_index < m_lock->skeleton_flow_params.size())
            return std::next(m_lock->skeleton_flow_params.begin(), flow_index)->first;
        if (flow_index - m_lock->skeleton_flow_params.size() < m_lock->skin_flow_params.size())
            return std::next(m_lock->skin_flow_params.begin(), flow_index - m_lock->skeleton_flow_params.size())->first;
    }
    this->fail("it returned lines of flow " + std::to_string(flow_index) + ", which the filler was not given");
}

orca_infill_status FillLibrary::run(int32_t call, const Surface &surface, const FillParams &params, BuiltinInfillOutput &out)
{
    const BuiltinMirror::Request request(call, surface, params, *this, m_lock.get(),
                                         this->object_state != nullptr ? this->object_state->raw : nullptr);
    const orca_infill_status status = m_library->run(m_instance, &request.get(), out.sink());
    if (out.spacing)
        this->spacing = *out.spacing;
    return status;
}

void FillLibrary::check(orca_infill_status status, const BuiltinInfillOutput &out) const
{
    if (status == ORCA_INFILL_OK)
        return;
    if (status == ORCA_INFILL_ERR_FAILED)
        throw InfillFailedException();
    if (status == ORCA_INFILL_ERR_NOMEM)
        throw std::bad_alloc();
    this->fail(status == ORCA_INFILL_ERR_STATE ? std::string("it runs under another scaling factor") :
               status == ORCA_INFILL_ERR_EXCEPTION && ! out.error.empty() ? out.error :
                                                                           "error " + std::to_string(status));
}

void FillLibrary::fail(const std::string &reason) const
{
    const InfillPatternDescriptor &pattern = infill_pattern(m_pattern);
    throw SlicingError(format(_u8L("The infill pattern \"%1%\" of the pattern library %2% failed: %3%"),
                              pattern.key != nullptr ? pattern.key : m_file.c_str(), m_file, reason));
}

} // namespace Slic3r
