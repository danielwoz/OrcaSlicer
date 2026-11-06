#ifndef slic3r_FillLightning_hpp_
#define slic3r_FillLightning_hpp_

#include "libslic3r/ExPolygon.hpp"
#include "FillBase.hpp"
#include <memory>
#include <functional>
#include <utility>
#include "libslic3r/Polyline.hpp"

namespace Slic3r { class Point; }

namespace Slic3r {

namespace FillLightning {

class Generator;

class Filler : public Slic3r::Fill
{
public:
    ~Filler() override = default;
    bool is_self_crossing() override { return false; }

    const Generator *generator { nullptr };
protected:
    Fill* clone() const override { return new Filler(*this); }

    void _fill_surface_single(const FillParams              &params,
                              unsigned int                   thickness_layers,
                              const std::pair<float, Point> &direction,
                              ExPolygon                      expolygon,
                              Polylines &polylines_out) override;

    // Let the G-code export reoder the infill lines.
	bool no_sort() const override { return false; }
};

} // namespace FillAdaptive
} // namespace Slic3r

#endif // slic3r_FillLightning_hpp_
