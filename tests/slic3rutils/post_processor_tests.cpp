// Orca #17095: a post-processing script or plugin that fails fails the export or upload, not the slice,
// so the plate stays valid and Slice / Print can be retried.

#include <catch2/catch.hpp>

#include <exception>
#include <fstream>
#include <string>
#include <utility>

#include <boost/filesystem.hpp>
#include <wx/event.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"

using namespace Slic3r;

namespace {

// Runs `script` as the post-processing script on a real G-code file, so a failure comes from the script
// and not from a missing input, and returns what it threw.
std::exception_ptr post_process_with(const std::string& script)
{
    const boost::filesystem::path gcode = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("pp_test_%%%%-%%%%.gcode");
    {
        std::ofstream out(gcode.string());
        out << "G1 X10 Y10\n";
    }
    DynamicPrintConfig config;
    config.set_key_value("post_process", new ConfigOptionStrings({script}));
    std::string path        = gcode.string();
    std::string output_name = gcode.string();
    std::exception_ptr ex;
    try {
        run_post_process_scripts(path, false, "File", output_name, config);
    } catch (...) {
        ex = std::current_exception();
    }
    boost::system::error_code ec;
    boost::filesystem::remove(gcode, ec);
    return ex;
}

SlicingProcessCompletedEvent completed(SlicingProcessCompletedEvent::StatusType status, std::exception_ptr ex)
{
    return SlicingProcessCompletedEvent(wxEVT_NULL, 0, status, std::move(ex));
}

} // namespace

TEST_CASE("A post-processing script that cannot run fails the export, not the slice", "[PostProcessor]")
{
    // Quoted, as a configured path with spaces must be (the temp directory may have some).
    const boost::filesystem::path missing = boost::filesystem::temp_directory_path() / "no_such_post_process_script.sh";
    const std::exception_ptr      ex      = post_process_with("\"" + missing.string() + "\"");

    REQUIRE(ex);
    CHECK_THROWS_AS(std::rethrow_exception(ex), ExportError);
    const SlicingProcessCompletedEvent evt = completed(SlicingProcessCompletedEvent::Error, ex);
    CHECK(evt.export_failed());
    CHECK_FALSE(evt.invalidate_plater());
}

TEST_CASE("A completion event tells a failed export from a failed slice", "[PostProcessor]")
{
    const SlicingProcessCompletedEvent export_error =
        completed(SlicingProcessCompletedEvent::Error, std::make_exception_ptr(ExportError("copy failed")));
    CHECK(export_error.export_failed());
    CHECK(export_error.critical_error());
    CHECK_FALSE(export_error.invalidate_plater());

    const SlicingProcessCompletedEvent slice_error =
        completed(SlicingProcessCompletedEvent::Error, std::make_exception_ptr(RuntimeError("slicing failed")));
    CHECK_FALSE(slice_error.export_failed());
    CHECK(slice_error.invalidate_plater());

    // A cancel or a success carries no exception to inspect.
    for (const auto status : {SlicingProcessCompletedEvent::Cancelled, SlicingProcessCompletedEvent::Finished}) {
        const SlicingProcessCompletedEvent evt = completed(status, nullptr);
        CHECK_FALSE(evt.export_failed());
        CHECK_FALSE(evt.invalidate_plater());
    }
}
