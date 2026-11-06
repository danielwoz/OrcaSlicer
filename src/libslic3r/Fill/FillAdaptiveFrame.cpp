// FillAdaptive::transform_to_world() and transform_to_octree(): the rotation of the octree of adaptive and support cubic
// infill. The print object rotates its mesh into the octree frame with them, and the pattern code its cubes back.
#include <cmath>

#include <Eigen/Geometry>

#include "../Geometry.hpp"
#include "../Point.hpp"
#include "FillAdaptive.hpp"

namespace Slic3r {
namespace FillAdaptive {

static constexpr double octree_rot[3] = { 5.0 * M_PI / 4.0, Geometry::deg2rad(215.264), M_PI / 6.0 };

Eigen::Quaterniond transform_to_world()
{
    return Eigen::AngleAxisd(octree_rot[2], Vec3d::UnitZ()) * Eigen::AngleAxisd(octree_rot[1], Vec3d::UnitY()) * Eigen::AngleAxisd(octree_rot[0], Vec3d::UnitX());
}

Eigen::Quaterniond transform_to_octree()
{
    return Eigen::AngleAxisd(- octree_rot[0], Vec3d::UnitX()) * Eigen::AngleAxisd(- octree_rot[1], Vec3d::UnitY()) * Eigen::AngleAxisd(- octree_rot[2], Vec3d::UnitZ());
}

} // namespace FillAdaptive
} // namespace Slic3r
