#include "ThumbnailData.hpp"
#include "libslic3r/BoundingBox.hpp"

namespace Slic3r {

void ThumbnailData::set(unsigned int w, unsigned int h)
{
    if ((w == 0) || (h == 0))
        return;

    if ((width != w) || (height != h))
    {
        width = w;
        height = h;
        // defaults to white texture
        pixels.clear();
        pixels = std::vector<unsigned char>(width * height * 4, 255);
    }
}

void ThumbnailData::reset()
{
    width = 0;
    height = 0;
    pixels.clear();
}

bool ThumbnailData::is_valid() const
{
    return (width != 0) && (height != 0) && ((unsigned int)pixels.size() == 4 * width * height);
}

BBoxData wipe_tower_first_layer_bbox_data(const Points &corners, const Vec2d &plate_origin, int plate_index, float first_layer_height)
{
    BBoxData data;
    data.name         = "wipe_tower";
    data.id           = plate_index + 1000;
    data.area         = 0.f;
    data.layer_height = first_layer_height;
    if (corners.empty())
        return data;
    // The box round the whole outline: a rib tower's outline (and a cone's ellipse) is not a
    // rectangle whose points 0 and 2 are opposite corners, and with a rotation no two points are
    // the box's corners.
    auto bb = unscaled(BoundingBox(corners));
    bb.min -= plate_origin;
    bb.max -= plate_origin;
    data.bbox = {bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()};
    return data;
}

} // namespace Slic3r
