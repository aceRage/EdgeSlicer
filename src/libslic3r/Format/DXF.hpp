#ifndef slic3r_Format_DXF_hpp_
#define slic3r_Format_DXF_hpp_

// Minimal ASCII DXF writer for 2D outlines (no dependency).
//
// Version: AutoCAD R12 (AC1009). It is the oldest ASCII flavour every CAD / laser / vector tool
// still reads (LibreCAD, FreeCAD, Fusion, Inkscape, LightBurn), it needs no entity handles, no
// OBJECTS section and no BLOCK_RECORD table, so there is very little that can be written wrong.
// Closed outlines are 2D POLYLINE + VERTEX + SEQEND entities with the "closed" flag (70 = 1);
// R12 has no LWPOLYLINE. $INSUNITS = 4 (millimetres) is written in the HEADER (newer readers
// honour it, R12 readers ignore it, size in drawing units is the same: 1 unit = 1 mm).
//
// Coordinates: ExPolygons use scaled integer coordinates (SCALING_FACTOR, 1 unit = 1 nm); they
// are written in millimetres. The Y axis is written as it is (DXF is Y up); the caller flips
// shapes which come from a Y down source (SVG).

#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ExPolygon.hpp"

namespace Slic3r {

// Layer name valid in R12: at most 31 characters of [A-Za-z0-9_$-], other characters become '_',
// an empty name becomes "LAYER". Used by export_dxf, public so that callers can predict names.
std::string dxf_sanitize_layer_name(const std::string &name);

// Content of the DXF file. Layers with the same name (after sanitizing) get a numeric suffix.
// Every layer has its own colour index (1..7, cycling). Contours and holes are all written as
// closed polylines on the layer of their group.
std::string dxf_to_string(const std::vector<std::pair<std::string, ExPolygons>> &layers);

// Write the file. false and *err (when not null) on failure.
bool export_dxf(const std::string &path, const std::vector<std::pair<std::string /*layer*/, ExPolygons>> &layers, std::string *err = nullptr);

} // namespace Slic3r

#endif // slic3r_Format_DXF_hpp_
