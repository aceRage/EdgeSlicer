// Why a send to a Bambu printer failed, in words a user can act on (BambuSendDiagnosis).
//
// A P1S owner hit "3120" and nothing else: EdgeSlicer's network plug-in has no cloud printing, so its
// start_print always answers -3120, and the send had gone to the cloud because the LAN route was
// never attempted (no microSD card, no IP address, no access code, cloud-only firmware) - or because
// the LAN attempt failed and the cloud fallback's -3120 replaced its error. What is pinned here is
// what the user reads: the real cause, the fix, the code support knows it by, and where the logs are.
//
// Nothing here touches wx, a plug-in, the network or a printer.

#include <catch2/catch.hpp>

#include <string>

#include "slic3r/GUI/BambuSendDiagnosis.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

using Slic3r::GUI::BambuLanFailure;
using Slic3r::GUI::BambuLanSkip;
using Slic3r::GUI::BambuSendFailure;
using Slic3r::GUI::BambuSendFailureText;
using Slic3r::GUI::bambu_classify_lan_failure;
using Slic3r::GUI::bambu_cloud_print_supported;
using Slic3r::GUI::bambu_lan_skip_reason;
using Slic3r::GUI::bambu_lan_skip_tag;
using Slic3r::GUI::bambu_log_dir_for_display;
using Slic3r::GUI::bambu_reply_is_auth_refusal;
using Slic3r::GUI::bambu_send_failure_text;

namespace {

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

// A cloud-bound printer on EdgeSlicer's own plug-in: no cloud to fall back to.
BambuSendFailure ultranet_print()
{
    BambuSendFailure f;
    f.cloud_supported = bambu_cloud_print_supported(true);
    f.ultranet_log    = true;
    f.log_dir         = "%APPDATA%\\EdgeSlicer\\log";
    return f;
}

BambuSendFailureText english(const BambuSendFailure& f) { return bambu_send_failure_text(f, false); }

} // namespace

TEST_CASE("LAN skip reasons keep PrintJob's params.comments order and tags", "[BambuSendDiagnosis]")
{
    CHECK(bambu_lan_skip_reason(true, false, true, true) == BambuLanSkip::None);
    // No IP address wins over everything: without it nothing else can be checked.
    CHECK(bambu_lan_skip_reason(false, true, false, false) == BambuLanSkip::NoIp);
    CHECK(bambu_lan_skip_reason(true, true, false, false) == BambuLanSkip::CloudOnlyFirmware);
    CHECK(bambu_lan_skip_reason(true, false, false, false) == BambuLanSkip::NoSdcard);
    CHECK(bambu_lan_skip_reason(true, false, true, false) == BambuLanSkip::NoAccessCode);

    CHECK(std::string(bambu_lan_skip_tag(BambuLanSkip::None)).empty());
    CHECK(std::string(bambu_lan_skip_tag(BambuLanSkip::NoIp)) == "no_ip");
    CHECK(std::string(bambu_lan_skip_tag(BambuLanSkip::CloudOnlyFirmware)) == "low_version");
    CHECK(std::string(bambu_lan_skip_tag(BambuLanSkip::NoSdcard)) == "no_sdcard");
    CHECK(std::string(bambu_lan_skip_tag(BambuLanSkip::NoAccessCode)) == "no_password");
}

TEST_CASE("Only a plug-in other than UltraNet has a cloud to fall back to", "[BambuSendDiagnosis]")
{
    CHECK_FALSE(bambu_cloud_print_supported(true));
    CHECK(bambu_cloud_print_supported(false));
}

TEST_CASE("The printer's 'verify failed' reply is recognised as a refusal", "[BambuSendDiagnosis]")
{
    CHECK(bambu_reply_is_auth_refusal("failed", "mqtt message verify failed"));
    CHECK(bambu_reply_is_auth_refusal("FAIL", "MQTT message verify failed"));
    CHECK(bambu_reply_is_auth_refusal("fail", "command verification failed"));
    CHECK_FALSE(bambu_reply_is_auth_refusal("success", "mqtt message verify failed"));
    CHECK_FALSE(bambu_reply_is_auth_refusal("failed", "file not found"));
    CHECK_FALSE(bambu_reply_is_auth_refusal("", ""));
}

TEST_CASE("LAN failures are classified from the code, the plug-in's text and the printer's reply", "[BambuSendDiagnosis]")
{
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED, "", false) == BambuLanFailure::Upload);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, "", false) == BambuLanFailure::Upload);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED, "", false) == BambuLanFailure::Upload);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, "", false) == BambuLanFailure::Publish);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_CONNECT_FAILED, "", false) == BambuLanFailure::Connect);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED, "", false) == BambuLanFailure::Connect);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE, "", false) == BambuLanFailure::FileTooLarge);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_WR_FILE_NOT_EXIST, "", false) == BambuLanFailure::FileMissing);
    CHECK(bambu_classify_lan_failure(-9999, "", false) == BambuLanFailure::Other);

    // The text says more than the code.
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED, "530 Login incorrect.", false) ==
          BambuLanFailure::AccessCode);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_CONNECT_FAILED, "Connection refused: not authorised", false) ==
          BambuLanFailure::AccessCode);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED, "Connection timed out", false) ==
          BambuLanFailure::Connect);
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, "mqtt message verify failed", false) ==
          BambuLanFailure::Refused);
    // And the printer's own refusal beats all of it.
    CHECK(bambu_classify_lan_failure(BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, "", true) == BambuLanFailure::Refused);
}

TEST_CASE("LAN never attempted, no cloud: the reason, the fix, -3120 and the main log", "[BambuSendDiagnosis]")
{
    BambuSendFailure f = ultranet_print();

    SECTION("no microSD card (the P1S report)")
    {
        f.lan_skip                  = BambuLanSkip::NoSdcard;
        const BambuSendFailureText t = english(f);
        CHECK(t.code == BAMBU_NETWORK_ERR_PRINT_SP_POST_TASK_FAILED);
        CHECK(contains(t.headline, "microSD"));
        CHECK(contains(t.headline, "-3120"));
        CHECK(contains(t.headline, "no_sdcard"));
        CHECK(contains(t.detail, "P1 and A1"));
        CHECK(contains(t.detail, "local network only"));
        // The LAN path never ran, so there is no ultranet_print.log to look for.
        CHECK_FALSE(contains(t.where, "ultranet_print.log"));
        CHECK(contains(t.where, "\"print_job:\""));
        CHECK(contains(t.where, "%APPDATA%\\EdgeSlicer\\log"));
    }
    SECTION("no IP address")
    {
        f.lan_skip                  = BambuLanSkip::NoIp;
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "no_ip"));
        CHECK(contains(t.detail, "guest Wi-Fi"));
        CHECK(contains(t.detail, "VLAN"));
        CHECK(contains(t.detail, "VPN"));
        CHECK(contains(t.detail, "2021"));
        CHECK(contains(t.detail, "1990"));
    }
    SECTION("no access code")
    {
        f.lan_skip                  = BambuLanSkip::NoAccessCode;
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "no_password"));
        CHECK(contains(t.headline, "access code"));
        CHECK(contains(t.detail, "Device tab"));
    }
    SECTION("cloud-only firmware")
    {
        f.lan_skip                  = BambuLanSkip::CloudOnlyFirmware;
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "low_version"));
        CHECK(contains(t.detail, "firmware"));
    }
}

TEST_CASE("LAN attempted and failed, no cloud: the LAN error, not -3120", "[BambuSendDiagnosis]")
{
    BambuSendFailure f = ultranet_print();

    SECTION("FTP upload failed")
    {
        f.lan_code                  = BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED;
        const BambuSendFailureText t = english(f);
        CHECK(t.code == BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED);
        CHECK(contains(t.headline, "-2130"));
        CHECK_FALSE(contains(t.full(), "-3120"));
        CHECK(contains(t.detail, "990"));
        CHECK(contains(t.detail, "no Bambu cloud fallback"));
        CHECK(contains(t.where, "ultranet_print.log"));
    }
    SECTION("could not connect")
    {
        f.lan_code                  = BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "could not connect"));
        CHECK(contains(t.detail, "8883"));
        CHECK(contains(t.detail, "990"));
    }
    SECTION("access code rejected")
    {
        f.lan_code                  = BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED;
        f.lan_detail                = "530 Login incorrect.";
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "rejected the access code"));
    }
    SECTION("uploaded but the command did not take: Developer Mode is the likely cause")
    {
        f.lan_code                  = BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "-4030"));
        CHECK(contains(t.detail, "LAN Only Mode"));
        CHECK(contains(t.detail, "Developer Mode"));
        CHECK(contains(t.detail, "likely"));
    }
    SECTION("the printer refused the command")
    {
        f.lan_code                  = BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        f.printer_refused           = true;
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "refused"));
        CHECK(contains(t.detail, "LAN Only Mode"));
        CHECK(contains(t.detail, "Developer Mode"));
    }
    SECTION("an unknown code still reads, and carries the plug-in's words")
    {
        f.lan_code                  = -9999;
        f.lan_detail                = "something odd";
        const BambuSendFailureText t = english(f);
        CHECK(contains(t.headline, "-9999"));
        CHECK(contains(t.detail, "something odd"));
    }
}

TEST_CASE("With a plug-in that has a cloud, a -3120 from it is reported next to the LAN cause", "[BambuSendDiagnosis]")
{
    BambuSendFailure f;
    f.log_dir     = "/home/u/.config/EdgeSlicer/log";
    f.lan_skip    = BambuLanSkip::NoSdcard;
    f.cloud_tried = true;
    f.cloud_code  = BAMBU_NETWORK_ERR_PRINT_SP_POST_TASK_FAILED;
    const BambuSendFailureText t = english(f);
    CHECK(t.code == BAMBU_NETWORK_ERR_PRINT_SP_POST_TASK_FAILED);
    CHECK(contains(t.detail, "cloud instead failed as well"));
    CHECK_FALSE(contains(t.detail, "local network only"));

    BambuSendFailure lan;
    lan.lan_code    = BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED;
    lan.cloud_tried = true;
    lan.cloud_code  = BAMBU_NETWORK_ERR_PRINT_SP_POST_TASK_FAILED;
    const BambuSendFailureText tl = english(lan);
    CHECK(tl.code == BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED);
    CHECK(contains(tl.detail, "-3120"));
}

TEST_CASE("A LAN-mode printer or an upload does not mention a cloud fallback", "[BambuSendDiagnosis]")
{
    BambuSendFailure f = ultranet_print();
    f.cloud_bound      = false;
    f.lan_code         = BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    CHECK_FALSE(contains(english(f).full(), "cloud"));

    BambuSendFailure up = ultranet_print();
    up.upload_only      = true;
    up.cloud_bound      = false;
    up.lan_skip         = BambuLanSkip::NoSdcard;
    up.skipped_code     = -1;
    up.log_prefix       = "send_job:";
    const BambuSendFailureText t = english(up);
    CHECK(t.code == -1);
    CHECK_FALSE(contains(t.full(), "cloud"));
    CHECK(contains(t.where, "\"send_job:\""));
}

TEST_CASE("Without a translation catalogue the translated text is the English one", "[BambuSendDiagnosis]")
{
    BambuSendFailure f = ultranet_print();
    f.lan_skip         = BambuLanSkip::NoSdcard;
    CHECK(bambu_send_failure_text(f, true).full() == bambu_send_failure_text(f, false).full());
}

TEST_CASE("The log folder is shown as %APPDATA% when it lives there", "[BambuSendDiagnosis]")
{
    CHECK(bambu_log_dir_for_display("C:\\Users\\ann\\AppData\\Roaming\\EdgeSlicer", "C:\\Users\\ann\\AppData\\Roaming") ==
          "%APPDATA%\\EdgeSlicer\\log");
    // Case and slash direction do not matter on Windows.
    CHECK(bambu_log_dir_for_display("c:/users/ann/appdata/roaming/EdgeSlicer", "C:\\Users\\ann\\AppData\\Roaming") ==
          "%APPDATA%\\EdgeSlicer\\log");
    // Only a whole folder name matches.
    CHECK(bambu_log_dir_for_display("C:\\Users\\ann\\AppData\\RoamingX\\EdgeSlicer", "C:\\Users\\ann\\AppData\\Roaming") ==
          "C:\\Users\\ann\\AppData\\RoamingX\\EdgeSlicer\\log");
    CHECK(bambu_log_dir_for_display("/home/ann/.config/EdgeSlicer", "") == "/home/ann/.config/EdgeSlicer/log");
    CHECK(bambu_log_dir_for_display("D:\\Portable\\EdgeSlicer\\data", "C:\\Users\\ann\\AppData\\Roaming") ==
          "D:\\Portable\\EdgeSlicer\\data\\log");
}

// Orca #10981: "Allow Abnormal Storage". The state numbers are MachineObject::SdcardState.
TEST_CASE("Abnormal storage is usable for a LAN send only when the user allowed it", "[BambuSendDiagnosis][Storage]")
{
    using Slic3r::GUI::BambuStorageIssue;
    using Slic3r::GUI::bambu_storage_issue;
    using Slic3r::GUI::bambu_storage_usable_for_lan;

    CHECK(bambu_storage_issue(0, false) == BambuStorageIssue::Missing);
    CHECK(bambu_storage_issue(0, true) == BambuStorageIssue::Missing);
    CHECK(bambu_storage_issue(1, false) == BambuStorageIssue::None);
    CHECK(bambu_storage_issue(1, true) == BambuStorageIssue::None);
    CHECK(bambu_storage_issue(2, false) == BambuStorageIssue::Abnormal);
    CHECK(bambu_storage_issue(2, true) == BambuStorageIssue::None);
    // Read-only storage can never take an upload, whatever the setting.
    CHECK(bambu_storage_issue(3, false) == BambuStorageIssue::ReadOnly);
    CHECK(bambu_storage_issue(3, true) == BambuStorageIssue::ReadOnly);
    CHECK(bambu_storage_issue(4, true) == BambuStorageIssue::Unknown);
    CHECK(bambu_storage_issue(-1, true) == BambuStorageIssue::Unknown);

    CHECK(bambu_storage_usable_for_lan(1, false));
    CHECK_FALSE(bambu_storage_usable_for_lan(2, false));
    CHECK(bambu_storage_usable_for_lan(2, true));
    CHECK_FALSE(bambu_storage_usable_for_lan(3, true));
    CHECK_FALSE(bambu_storage_usable_for_lan(0, true));

    // The default (setting off) keeps the old behaviour exactly: only a normal card passes.
    for (int state = -1; state <= 4; ++state)
        CHECK(bambu_storage_usable_for_lan(state, false) == (state == 1));
}

// Secured mode: a Bambu printer with Developer Mode off rejects every unsigned control command.
// print.fun bit 29 tells the two modes apart; unknown (no report yet) must never count as secured.
TEST_CASE("print.fun bit 29 tells secured mode from Developer Mode", "[BambuSendDiagnosis][Secured]")
{
    using Slic3r::GUI::bambu_fun_secured_bit;

    bool secured = false;
    REQUIRE(bambu_fun_secured_bit("20000000", secured));
    CHECK(secured);                       // bit 29 set: 0x20000000
    REQUIRE(bambu_fun_secured_bit("1FFFFFFF", secured));
    CHECK_FALSE(secured);                 // every lower bit set, bit 29 clear
    REQUIRE(bambu_fun_secured_bit("0", secured));
    CHECK_FALSE(secured);
    REQUIRE(bambu_fun_secured_bit("1FFFFFFFFFFFFFFF", secured)); // bits 30 and up do not matter...
    CHECK(secured);                                              // ...but 0x1FFF... has bit 29 set
    REQUIRE(bambu_fun_secured_bit("DFFFFFFFDFFFFFFF", secured)); // bit 29 cleared in a mostly-full mask
    CHECK_FALSE(secured);
    REQUIRE(bambu_fun_secured_bit("3ffeffff", secured)); // lower-case hex digits read the same
    CHECK(secured);

    // Not known: the flag is left alone and the call says so.
    secured = false;
    CHECK_FALSE(bambu_fun_secured_bit("", secured));
    CHECK_FALSE(bambu_fun_secured_bit("zz", secured));
    CHECK_FALSE(bambu_fun_secured_bit("-1", secured));
    CHECK_FALSE(secured);
    secured = true;
    CHECK_FALSE(bambu_fun_secured_bit("", secured));
    CHECK(secured);
}

TEST_CASE("The firmware's unsigned-command refusal is recognised", "[BambuSendDiagnosis][Secured]")
{
    using Slic3r::GUI::bambu_err_code_is_unsigned_refusal;
    using Slic3r::GUI::bambu_reply_is_unsigned_refusal;

    CHECK_FALSE(bambu_err_code_is_unsigned_refusal(84033542));
    for (int code = 84033543; code <= 84033548; ++code)
        CHECK(bambu_err_code_is_unsigned_refusal(code));
    CHECK_FALSE(bambu_err_code_is_unsigned_refusal(84033549));
    CHECK_FALSE(bambu_err_code_is_unsigned_refusal(0));

    // The reply the owner's H2C sent to extrusion_cali_get.
    CHECK(bambu_reply_is_unsigned_refusal(84033543, "failed", "mqtt message verify failed"));
    CHECK(bambu_reply_is_unsigned_refusal(0, "failed", "mqtt message verify failed")); // wording alone
    CHECK(bambu_reply_is_unsigned_refusal(84033545, "fail", ""));                       // code alone
    CHECK_FALSE(bambu_reply_is_unsigned_refusal(84033543, "success", ""));              // not a failure
    CHECK_FALSE(bambu_reply_is_unsigned_refusal(0, "failed", "nozzle too cold"));
    CHECK_FALSE(bambu_reply_is_unsigned_refusal(0, "", ""));
}

TEST_CASE("Only control namespaces need a signature; reads still go out", "[BambuSendDiagnosis][Secured]")
{
    using Slic3r::GUI::bambu_command_needs_signature;

    CHECK(bambu_command_needs_signature("print", "extrusion_cali_get"));
    CHECK(bambu_command_needs_signature("print", "project_file"));
    CHECK(bambu_command_needs_signature("print", "get_auto_nozzle_mapping"));
    CHECK(bambu_command_needs_signature("system", "ledctrl"));
    CHECK(bambu_command_needs_signature("upgrade", "start"));
    CHECK_FALSE(bambu_command_needs_signature("pushing", "pushall"));
    CHECK_FALSE(bambu_command_needs_signature("info", "get_version"));
    CHECK_FALSE(bambu_command_needs_signature("system", "get_access_code"));
    CHECK_FALSE(bambu_command_needs_signature("", ""));
}

TEST_CASE("The secured-mode message is the one agreed wording", "[BambuSendDiagnosis][Secured]")
{
    CHECK(Slic3r::GUI::bambu_secured_mode_message(false) ==
          "This printer is not in Developer Mode, so it rejects commands from EdgeSlicer. "
          "Turn on LAN Only + Developer Mode on the printer.");
}
