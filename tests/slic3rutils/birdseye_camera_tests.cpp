// BirdsEye camera (BirdsEyeCamera.hpp): what EdgeSlicer reads from a Bambu printer's push about its
// live-view cameras, which camera a swap asks for, and the exact command JSON it publishes.
// Design: tests/design_birds_eye_camera.md. Nothing here touches wx, a plug-in, the network or a printer.

#include <catch2/catch.hpp>

#include "slic3r/GUI/BirdsEyeCamera.hpp"
#include "slic3r/GUI/BambuSendDiagnosis.hpp"

#include <string>
#include <vector>

using namespace Slic3r::GUI::BirdsEye;
using nlohmann::json;

// ---- push parsing ----------------------------------------------------------------------------------

TEST_CASE("BirdsEye: an H2-style ipcam lists both cameras", "[BirdsEye]")
{
    const json ipcam = json::parse(R"({
        "ipcam_dev": "1", "ipcam_record": "enable", "timelapse": "disable", "resolution": "1080p",
        "liveview": {"local": "rtsps", "remote": "none"},
        "current_liveview": 5,
        "support_liveview_cams": [4, 5],
        "support_record_cams": [4, 5],
        "enable_record_cams": [4]
    })");
    const IpcamCameras c = parse_ipcam_cameras(ipcam);
    CHECK(c.current == 5);
    CHECK(c.has_liveview);
    CHECK(c.liveview == std::vector<int>{4, 5});
    CHECK(c.has_record);
    CHECK(c.record == std::vector<int>{4, 5});
    CHECK(c.has_enabled);
    CHECK(c.enabled == std::vector<int>{4});
    CHECK(has_birdseye(c.liveview));
}

TEST_CASE("BirdsEye: an older printer's ipcam says nothing about cameras", "[BirdsEye]")
{
    const IpcamCameras c = parse_ipcam_cameras(json::parse(R"({"ipcam_dev": "1", "resolution": "1080p"})"));
    CHECK(c.current == CAMERA_UNKNOWN);
    CHECK_FALSE(c.has_liveview);
    CHECK_FALSE(c.has_record);
    CHECK_FALSE(c.has_enabled);
    CHECK(c.liveview.empty());
}

TEST_CASE("BirdsEye: one listed camera is no BirdsEye camera", "[BirdsEye]")
{
    const IpcamCameras c = parse_ipcam_cameras(json::parse(R"({"current_liveview": 4, "support_liveview_cams": [4]})"));
    CHECK(c.has_liveview);
    CHECK_FALSE(has_birdseye(c.liveview));
    CHECK_FALSE(has_birdseye({}));
}

TEST_CASE("BirdsEye: odd shapes in the push are skipped, never thrown on", "[BirdsEye]")
{
    SECTION("non-integer entries are dropped, unknown ids kept")
    {
        const IpcamCameras c = parse_ipcam_cameras(json::parse(R"({"support_liveview_cams": [4, "5", null, 7, 1.5]})"));
        CHECK(c.has_liveview);
        CHECK(c.liveview == std::vector<int>{4, 7});
    }
    SECTION("a list that is not an array leaves the caller's list alone")
    {
        const IpcamCameras c = parse_ipcam_cameras(json::parse(R"({"support_liveview_cams": "4,5", "enable_record_cams": {}})"));
        CHECK_FALSE(c.has_liveview);
        CHECK_FALSE(c.has_enabled);
    }
    SECTION("a current_liveview that is not a number reads as unknown")
    {
        CHECK(parse_ipcam_cameras(json::parse(R"({"current_liveview": "5"})")).current == CAMERA_UNKNOWN);
    }
    SECTION("an empty array is a real answer: no cameras")
    {
        const IpcamCameras c = parse_ipcam_cameras(json::parse(R"({"enable_record_cams": []})"));
        CHECK(c.has_enabled);
        CHECK(c.enabled.empty());
    }
    SECTION("not an object at all")
    {
        CHECK_NOTHROW(parse_ipcam_cameras(json()));
        CHECK_NOTHROW(parse_ipcam_cameras(json::array({4, 5})));
        CHECK_FALSE(parse_ipcam_cameras(json("ipcam")).has_liveview);
    }
}

// ---- which camera a swap asks for --------------------------------------------------------------------

TEST_CASE("BirdsEye: the swap goes to the next listed camera and wraps round", "[BirdsEye]")
{
    const std::vector<int> cams{4, 5};
    CHECK(next_camera(cams, 4) == 5);
    CHECK(next_camera(cams, 5) == 4);

    const std::vector<int> three{4, 5, 9};
    CHECK(next_camera(three, 5) == 9);
    CHECK(next_camera(three, 9) == 4);
}

TEST_CASE("BirdsEye: an unknown current camera swaps to the first listed one", "[BirdsEye]")
{
    CHECK(next_camera({4, 5}, CAMERA_UNKNOWN) == 4);
    CHECK(next_camera({4, 5}, 7) == 4);
    CHECK(next_camera({5, 4}, CAMERA_UNKNOWN) == 5);
    CHECK(next_camera({}, 4) == CAMERA_UNKNOWN);
}

TEST_CASE("BirdsEye: camera names", "[BirdsEye]")
{
    CHECK(camera_kind(4) == CameraKind::Main);
    CHECK(camera_kind(5) == CameraKind::BirdsEye);
    CHECK(camera_kind(6) == CameraKind::Other);
    CHECK(camera_label(4) == "Live View");
    CHECK(camera_label(5) == "BirdsEye");
    CHECK(camera_label(7) == "Camera 7");
}

// ---- the command published -------------------------------------------------------------------------

TEST_CASE("BirdsEye: the switch command is EdgeSlicer's own, never Studio's", "[BirdsEye]")
{
    const json j = switch_camera_command(5, "20001");
    REQUIRE(j.contains("camera"));
    REQUIRE(j.size() == 1);
    const json& c = j["camera"];
    CHECK(c["command"] == "change_liveview_cam_idx");
    CHECK(c["sequence_id"] == "20001");
    CHECK(c["sequence_id"].is_string());
    CHECK(c["camera_id"] == 5);
    CHECK(c["camera_id"].is_number_integer());
    CHECK(c["version"] == 1);
    CHECK(c["peer_name"] == "edgeslicer");
    CHECK(c["peer_name"] != "studio");
    CHECK(c.size() == 5);
    CHECK(j.dump() ==
          R"({"camera":{"camera_id":5,"command":"change_liveview_cam_idx","peer_name":"edgeslicer","sequence_id":"20001","version":1}})");
}

TEST_CASE("BirdsEye: the switch command can go without a peer name", "[BirdsEye]")
{
    // The fallback D3 names if the printer refuses "edgeslicer": the same command with the field left out.
    const json c = switch_camera_command(4, "7", "")["camera"];
    CHECK_FALSE(c.contains("peer_name"));
    CHECK(c["camera_id"] == 4);
    CHECK(c["version"] == 1);
}

TEST_CASE("BirdsEye: camera commands need a signature on a secured printer", "[BirdsEye]")
{
    // So publish_json never sends them to a printer outside LAN Only + Developer Mode (D6).
    CHECK(Slic3r::GUI::bambu_command_needs_signature("camera", "change_liveview_cam_idx"));
    CHECK(Slic3r::GUI::bambu_command_needs_signature("camera", "ipcam_record_set"));
}
