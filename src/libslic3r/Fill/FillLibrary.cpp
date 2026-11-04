#include "FillLibrary.hpp"

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
    this->run(ORCA_INFILL_CALL_FILL_SURFACE, *surface, params, out);
    return std::move(out.polylines);
}

ThickPolylines FillLibrary::fill_surface_arachne(const Surface *surface, const FillParams &params)
{
    BuiltinInfillOutput out;
    this->run(ORCA_INFILL_CALL_FILL_ARACHNE, *surface, params, out);
    return std::move(out.thick_polylines);
}

void FillLibrary::set_lock_region_param(const LockRegionParam &lock_param) { m_lock = std::make_unique<LockRegionParam>(lock_param); }

void FillLibrary::run(int32_t call, const Surface &surface, const FillParams &params, BuiltinInfillOutput &out)
{
    const BuiltinMirror::Request request(call, surface, params, *this, m_lock.get(),
                                         this->object_state != nullptr ? this->object_state->raw : nullptr);
    const orca_infill_status status = m_library->run(m_instance, &request.get(), out.sink());
    if (out.spacing)
        this->spacing = *out.spacing;
    if (status == ORCA_INFILL_OK)
        return;
    if (status == ORCA_INFILL_ERR_FAILED)
        throw InfillFailedException();
    if (status == ORCA_INFILL_ERR_NOMEM)
        throw std::bad_alloc();
    const InfillPatternDescriptor &pattern = infill_pattern(m_pattern);
    const std::string reason = status == ORCA_INFILL_ERR_STATE ? std::string("it runs under another scaling factor") :
                               status == ORCA_INFILL_ERR_EXCEPTION && ! out.error.empty() ? out.error :
                                                                                           "error " + std::to_string(status);
    throw SlicingError(format(_u8L("The infill pattern \"%1%\" of the pattern library %2% failed: %3%"),
                              pattern.key != nullptr ? pattern.key : m_file.c_str(), m_file, reason));
}

} // namespace Slic3r
