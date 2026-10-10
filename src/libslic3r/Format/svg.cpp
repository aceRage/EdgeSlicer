#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"

#include "svg.hpp"
#include "nanosvg/nanosvg.h"
#include "../Emboss.hpp"          // polygons2model, ProjectZ
#include "../NSVGUtils.hpp"       // size and complexity limits for an SVG from outside, shapes
#include "../UntrustedInput.hpp"

#include <memory>
#include <string>

#include <boost/log/trivial.hpp>

// .svg loaded as a model (Model::read_from_file): the command line, and the GUI when it loads an
// .svg given on its command line. File > Import and drag & drop in the GUI create SVGs by the SVG
// gizmo instead ("SVG" / "SVG (Split)").
//
// One part per painted fill and per stroke (in the order of the file), each extruded 10 mm, named
// part_<n>, as Bambu Studio's loader. The parts keep their whole outlines (they can overlap).
//
// The shapes come from the same conversion as the SVG gizmo (create_shape_with_ids): fill rule,
// holes, islands inside of holes, several separate islands in one <path>, strokes as their outline
// and invisible shapes skipped. The former OpenCASCADE loader made one face per shape with its
// largest contour as the outline and every other contour as a hole, so separate islands (letters
// of a word in one <path>, islands inside of a hole) were lost, and a shape with fill and stroke
// lost the stroke.

namespace Slic3r {

namespace {
// Extrusion of every part [mm], as the former loader
const double SVG_LOAD_DEPTH = 10.;
// Curves are flattened with the same tolerance as the SVG gizmo (0.1 mm, squared and scaled)
const double SVG_LOAD_TOLERANCE = (0.1 * 0.1) / SCALING_FACTOR / SCALING_FACTOR;
} // namespace

bool load_svg(const char *path, Model *model, std::string &message)
{
    // The file is not ours: read it with a size limit and refuse a drawing with an absurd number of
    // shapes or points before anything is interpolated.
    bool too_large = false;
    std::unique_ptr<std::string> text = read_from_disk(path, untrusted::SVG_SIZE_LIMIT, &too_large);
    if (text == nullptr) {
        message = too_large ? "import svg failed: svg file is too large." : "import svg failed: could not open svg.";
        return false;
    }
    SvgRefusal  refusal = SvgRefusal::None;
    std::string why;
    NSVGimage_ptr image = nsvgParse_checked(*text, refusal, &why);
    if (image == nullptr) {
        message = (refusal == SvgRefusal::TooComplex) ? "import svg failed: svg is too complex (" + why + ")."
                                                      : "import svg failed: could not open svg.";
        return false;
    }
    if (image->shapes == nullptr) {
        message = "import svg failed: could not parse imported svg data.";
        return false;
    }

    NSVGLineParams params{SVG_LOAD_TOLERANCE};
    params.max_flat_points = untrusted::SVG_MAX_FLAT_POINTS;
    bool              too_complex = false;
    // keep the coordinates of the drawing (mm, Y up), the object is placed by the caller
    ExPolygonsWithIds shapes = create_shape_with_ids(*image, params, &too_complex, false);
    if (too_complex) {
        message = "import svg failed: svg is too complex (too many points).";
        return false;
    }

    const Emboss::ProjectScale project(std::make_unique<Emboss::ProjectZ>(SVG_LOAD_DEPTH / SCALING_FACTOR), SCALING_FACTOR);
    std::vector<std::pair<std::string, TriangleMesh>> parts;
    int name_index = 1;
    for (const ExPolygonsWithId &shape : shapes) {
        if (shape.expoly.empty())
            continue;
        indexed_triangle_set its = Emboss::polygons2model(shape.expoly, project);
        if (its.indices.empty())
            continue;
        parts.emplace_back("part_" + std::to_string(name_index++), TriangleMesh(std::move(its)));
    }
    if (parts.empty()) {
        message = "import svg failed: svg does not contain a single shape.";
        return false;
    }

    ModelObject *new_object = model->add_object();
    new_object->input_file  = path;
    for (auto &[name, mesh] : parts) {
        ModelVolume *new_volume       = new_object->add_volume(std::move(mesh));
        new_volume->name              = name;
        new_volume->source.input_file = path;
        new_volume->source.object_idx = (int) model->objects.size() - 1;
        new_volume->source.volume_idx = (int) new_object->volumes.size() - 1;
    }
    BOOST_LOG_TRIVIAL(info) << "load_svg: " << parts.size() << " parts from " << path;
    return true;
}

} // namespace Slic3r
