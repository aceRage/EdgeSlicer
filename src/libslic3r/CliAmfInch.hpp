#pragma once
#ifndef slic3r_CliAmfInch_hpp_
#define slic3r_CliAmfInch_hpp_

// Model::read_from_file reports AMF inches through the same bool the CLI reads as
// "this is a Bambu 3MF" (is_bbl_3mf). An AMF in inches was therefore treated as a
// project and the CLI went on to read settings the file does not have (OrcaSlicer
// #17311 / BambuStudio #12965). Header-only so Catch2 can cover it.

#include "Model.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <string>

namespace Slic3r {

inline bool cli_path_is_amf(const std::string &file)
{
    return boost::algorithm::iends_with(file, ".amf");
}

// For an AMF, convert imperial units when the reused flag is set, then always
// clear it so the file is not treated as a Bambu/Orca 3MF project.
inline void cli_apply_amf_inch_flag(const std::string &file, Model &model, bool &is_bbl_3mf)
{
    if (!cli_path_is_amf(file))
        return;
    if (is_bbl_3mf)
        model.convert_from_imperial_units(false);
    is_bbl_3mf = false;
}

} // namespace Slic3r

#endif // slic3r_CliAmfInch_hpp_
