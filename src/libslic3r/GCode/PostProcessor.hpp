#ifndef slic3r_GCode_PostProcessor_hpp_
#define slic3r_GCode_PostProcessor_hpp_

#include <string>
#include <vector>

#include "../libslic3r.h"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Run post processing script / scripts if defined.
// Returns true if a post-processing script was executed.
// Returns false if no post-processing script was defined.
// Throws an exception on error.
// host is one of "File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...
// If make_copy, then a temp file will be created for src_path by adding a ".pp" suffix and src_path will be updated.
// In that case the caller is responsible to delete the temp file created.
// output_name is the final name of the G-code on SD card or when uploaded to PrusaLink or OctoPrint.
// If uploading to PrusaLink or OctoPrint, then the file will be renamed to output_name first on the target host.
// The post-processing script may change the output_name.
extern bool run_post_process_scripts(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config);

inline bool run_post_process_scripts(std::string &src_path, const DynamicPrintConfig &config)
{
	std::string src_path_name = src_path;
	return run_post_process_scripts(src_path, false, "File", src_path_name, config);
}

// BBS
extern void gcode_add_line_number(const std::string &path, const DynamicPrintConfig &config);

// True when G-code packed into a .gcode.3mf should run post-processing scripts.
// with_gcode / skip_static are the SaveStrategy bits: backups (SkipStatic) do not pack G-code.
// BBL printers already post-process the mapped temp file after slicing, so they are skipped.
bool should_post_process_packed_gcode(bool with_gcode, bool skip_static, PrinterTechnology tech, bool is_bbl_printer);

// One plate's G-code path (rewritten to the post-processed copy when a script ran) and the
// Print's full_print_config. Untrusted 3MF scripts must already have been stripped from that
// config (UntrustedSettingsGuard / neutralize_settings, Edge #123); this does not re-read a 3MF.
struct PackedGcodePlate
{
    std::string              *gcode_file { nullptr };
    const DynamicPrintConfig *config { nullptr };
};

// Run post-processing on a copy of each plate's G-code (the viewer's mapping stays on the
// original). Host is always "File"; a script rename of output_name is ignored for 3mf.
// Appends copy paths to tmp_files as they are created so the caller can delete them on
// success and on exception. Throws RuntimeError if a script fails.
void post_process_packed_gcodes(std::vector<PackedGcodePlate> &plates, const std::string &output_name, std::vector<std::string> &tmp_files);

} // namespace Slic3r

#endif /* slic3r_GCode_PostProcessor_hpp_ */
