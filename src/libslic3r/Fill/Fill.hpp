#ifndef slic3r_Fill_hpp_
#define slic3r_Fill_hpp_

#include <memory.h>
#include <float.h>
#include <stdint.h>

#include "../libslic3r.h"
#include "../PrintConfig.hpp"

#include "../Line.hpp"

#include "FillBase.hpp"

namespace Slic3r {

class ExtrusionEntityCollection;
class LayerRegion;
class PrintObject;

// Orca: Share the layer rotation calculation between infill generation and internal
// bridge angle selection so both interpret rotation templates in the same way.
double calculate_infill_rotation_angle(const PrintObject *object, size_t layer_id,
                                      const double &fixed_infill_angle, const std::string &template_string);

// Detect narrow internal solid infill: of the lines a solid area would be filled with, given section by section across
// it (each line from its lower end a to its upper end b, all the lines of a section side by side at one x), removes the
// short ones that would be laid one after another. What is left is filled with lines, the rest with loops.
// Declared here for the unit tests.
std::vector<Lines> filter_vibrating_extrusions(const std::vector<Lines> &lines_sections);

// An interface class to Perl, aggregating an instance of a Fill and a FillData.
class Filler
{
public:
    Filler() : fill(nullptr) {}
    ~Filler() { 
        delete fill; 
        fill = nullptr;
    }
    Fill        *fill;
    FillParams   params;
};

} // namespace Slic3r

#endif // slic3r_Fill_hpp_
