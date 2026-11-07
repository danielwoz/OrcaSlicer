// EdgeGrid::save_png(): writes the signed distance field of a grid to a PNG file, for debugging.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "BoundingBox.hpp"
#include "EdgeGrid.hpp"
#include "PNGReadWrite.hpp"
#include "Point.hpp"
#include "libslic3r.h"

namespace Slic3r {

void EdgeGrid::save_png(const EdgeGrid::Grid &grid, const BoundingBox &bbox, coord_t resolution, const char *path, size_t scale)
{
    coord_t w = (bbox.max(0) - bbox.min(0) + resolution - 1) / resolution;
    coord_t h = (bbox.max(1) - bbox.min(1) + resolution - 1) / resolution;

	std::vector<uint8_t> pixels(w * h * 3, 0);

    const coord_t search_radius = grid.resolution() * 2;
	const coord_t display_blend_radius = grid.resolution() * 2;
	for (coord_t r = 0; r < h; ++r) {
    	for (coord_t c = 0; c < w; ++ c) {
			unsigned char *pxl = pixels.data() + (((h - r - 1) * w) + c) * 3;
			Point pt(c * resolution + bbox.min(0), r * resolution + bbox.min(1));
			coordf_t min_dist;
			bool on_segment = true;
			#if 0
			if (grid.signed_distance_edges(pt, search_radius, min_dist, &on_segment)) {
			#else
			if (grid.signed_distance(pt, search_radius, min_dist)) {
			#endif
				float s = 255 * std::abs(min_dist) / float(display_blend_radius);
				int is = std::max(0, std::min(255, int(floor(s + 0.5f))));
				if (min_dist < 0) {
					if (on_segment) {
						pxl[0] = 255;
						pxl[1] = 255 - is;
						pxl[2] = 255 - is;
					} else {
						pxl[0] = 255;
						pxl[1] = 0;
						pxl[2] = 255 - is;
					}
				}
				else {
					if (on_segment) {
						pxl[0] = 255 - is;
						pxl[1] = 255 - is;
						pxl[2] = 255;
					} else {
						pxl[0] = 255 - is;
						pxl[1] = 0;
						pxl[2] = 255;
					}
				}
			} else {
				pxl[0] = 0;
				pxl[1] = 255;
				pxl[2] = 0;
			}

			float gridx = float(pt(0) - grid.bbox().min(0)) / float(grid.resolution());
			float gridy = float(pt(1) - grid.bbox().min(1)) / float(grid.resolution());
			if (gridx >= -0.4f && gridy >= -0.4f && gridx <= grid.cols() + 0.4f && gridy <= grid.rows() + 0.4f) {
				int ix = int(floor(gridx + 0.5f));
				int iy = int(floor(gridy + 0.5f));
				float dx = gridx - float(ix);
				float dy = gridy - float(iy);
				float d = sqrt(dx*dx + dy*dy) * float(grid.resolution()) / float(resolution);
				if (d < 1.f) {
					// Less than 1 pixel from the grid point.
					float t = 0.5f + 0.5f * d;
					pxl[0] = (unsigned char)(t * pxl[0]);
					pxl[1] = (unsigned char)(t * pxl[1]);
					pxl[2] = (unsigned char)(t * pxl[2]);
				}
			}

			float dgrid = fabs(min_dist) / float(grid.resolution());
			float igrid = floor(dgrid + 0.5f);
			dgrid = std::abs(dgrid - igrid) * float(grid.resolution()) / float(resolution);
			if (dgrid < 1.f) {
				// Less than 1 pixel from the grid point.
				float t = 0.5f + 0.5f * dgrid;
				pxl[0] = (unsigned char)(t * pxl[0]);
				pxl[1] = (unsigned char)(t * pxl[1]);
				pxl[2] = (unsigned char)(t * pxl[2]);
				if (igrid > 0.f) {
					// Other than zero iso contour.
					int g = pxl[1] + 255.f * (1.f - t);
					pxl[1] = std::min(g, 255);
				}
			}
		}
    }

	png::write_rgb_to_file_scaled(path, w, h, pixels, scale);
}

} // namespace Slic3r
