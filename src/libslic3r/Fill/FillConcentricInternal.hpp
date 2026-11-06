#ifndef slic3r_FillConcentricInternal_hpp_
#define slic3r_FillConcentricInternal_hpp_

#include "FillBase.hpp"

namespace Slic3r {

class FillConcentricInternal : public Fill
{
public:
    ~FillConcentricInternal() override = default;
    // The variable width loops of each of Fill::no_overlap_expolygons, which Fill::append_concentric_internal_extrusions()
    // extrudes.
    ThickPolylines concentric_internal_lines(const FillParams &params);
    bool is_self_crossing() override { return false; }

protected:
    Fill* clone() const override { return new FillConcentricInternal(*this); };
    bool no_sort() const override { return true; }

    friend class Layer;
};

} // namespace Slic3r

#endif // slic3r_FillConcentricInternal_hpp_
