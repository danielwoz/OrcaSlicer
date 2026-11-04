#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FillBase.hpp"

struct orca_infill_builtin_library;
typedef int32_t orca_infill_status;

namespace Slic3r {

struct BuiltinInfillOutput;
struct InfillPatternDescriptor;

// The filler of a built-in pattern that its pattern library serves (Fill/InfillPatternLibraries.hpp). It holds one
// instance of the row's Fill class in the library, and every call passes all members of this filler and the
// LockRegionParam it was given, so that the instance computes what the class computes in libslic3r. The lines come
// back in the order generated, with Fill::spacing, InfillFailedException and std::bad_alloc as the class leaves them;
// any other failure is a SlicingError. fill_surface_extrusion() extrudes the lines of Locked Zag with the flows of its
// LockRegionParam, and is Fill's for every other row, which calls back into fill_surface() and fill_surface_arachne().
class FillLibrary : public Fill
{
public:
    // The factory of a row a pattern library serves; throws SlicingError while no library is bound to the row.
    static Fill* create(const InfillPatternDescriptor &pattern);

    // An instance in library, which passed the checks of the loader for pattern; file names it in errors.
    FillLibrary(InfillPattern pattern, const orca_infill_builtin_library &library, std::string file);
    // The members of other, with an instance of its own.
    FillLibrary(const FillLibrary &other);
    FillLibrary& operator=(const FillLibrary &) = delete;
    ~FillLibrary() override;

    Fill* clone() const override { return new FillLibrary(*this); }
    bool  no_sort() const override;
    bool  is_self_crossing() override { return false; }

    Polylines      fill_surface(const Surface *surface, const FillParams &params) override;
    ThickPolylines fill_surface_arachne(const Surface *surface, const FillParams &params) override;
    void           set_lock_region_param(const LockRegionParam &lock_param) override;
    void           fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) override;
    // FillLockedZag::fill_surface_locked_zag(): appends the lines of each flow, also those generated before an
    // InfillFailedException.
    void           fill_surface_locked_zag(const Surface *surface, const FillParams &params,
                                           std::vector<std::pair<Polylines, Flow>> &multi_width_polyline);

    InfillPattern      pattern() const { return m_pattern; }
    const std::string& file() const { return m_file; }

private:
    // Runs call (ORCA_INFILL_CALL_*) in the library and writes Fill::spacing back.
    orca_infill_status run(int32_t call, const Surface &surface, const FillParams &params, BuiltinInfillOutput &out);
    // Throws what a status other than ORCA_INFILL_OK stands for.
    void check(orca_infill_status status, const BuiltinInfillOutput &out) const;
    [[noreturn]] void fail(const std::string &reason) const;
    // The flow of a group of Locked Zag lines: a skeleton flow of the LockRegionParam, then a skin flow.
    const Flow& lock_flow(size_t flow_index) const;

    InfillPattern                      m_pattern;
    const orca_infill_builtin_library *m_library;
    std::string                        m_file;
    void                              *m_instance;
    std::unique_ptr<LockRegionParam>   m_lock;
};

} // namespace Slic3r
