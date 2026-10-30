#include <catch2/catch.hpp>

#include "slic3r/GUI/BindDetectGuard.hpp"

#include <stdexcept>

using BBL::detectResult;
using Slic3r::GUI::is_usable_serial;
using Slic3r::GUI::kBindDetectConnectFailed;
using Slic3r::GUI::kBindDetectManualEntry;
using Slic3r::GUI::vet_bind_detect_result;

TEST_CASE("bind_detect: a success with no serial never reaches insert_local_device", "[BindDetectGuard]")
{
    SECTION("the empty result the stub plug-in returned")
    {
        detectResult d;
        CHECK(vet_bind_detect_result(0, d) == kBindDetectManualEntry);
    }
    SECTION("a blank serial")
    {
        detectResult d;
        d.dev_id       = "   ";
        d.model_id     = "C12";
        d.connect_type = "lan";
        d.bind_state   = "free";
        CHECK(vet_bind_detect_result(0, d) == kBindDetectManualEntry);
    }
    SECTION("a serial with characters a serial never has")
    {
        for (const char *bad : {"01P00A#1", "01P/00A", "01P 00A", "device/+/x", "fake\nprinter"}) {
            detectResult d;
            d.dev_id = bad;
            CHECK(vet_bind_detect_result(0, d) == kBindDetectManualEntry);
        }
    }
    SECTION("a positive code with no serial is a success code too, and is refused the same way")
    {
        detectResult d;
        CHECK(vet_bind_detect_result(1, d) == kBindDetectManualEntry);
    }
}

TEST_CASE("bind_detect: failures pass through unchanged", "[BindDetectGuard]")
{
    for (int code : {-1, -2, -3, -4, -100}) {
        detectResult d;
        d.dev_id = "01P00A123456789";
        CHECK(vet_bind_detect_result(code, d) == code);
        CHECK(d.dev_id == "01P00A123456789");
    }
    detectResult d;
    CHECK(vet_bind_detect_result(kBindDetectConnectFailed, d) == kBindDetectConnectFailed);
}

TEST_CASE("bind_detect: a detected printer is normalised", "[BindDetectGuard]")
{
    SECTION("only a serial: name defaults to it, LAN and free assumed, model left to the serial prefix")
    {
        detectResult d;
        d.dev_id = " 01p00a123456789\r\n";
        REQUIRE(vet_bind_detect_result(0, d) == 0);
        CHECK(d.dev_id == "01P00A123456789");
        CHECK(d.dev_name == "01P00A123456789");
        CHECK(d.model_id.empty());
        CHECK(d.connect_type == "lan");
        CHECK(d.bind_state == "free");
    }
    SECTION("a full answer is kept, with the two states lower-cased")
    {
        detectResult d;
        d.dev_id       = "31B00C000000001";
        d.dev_name     = "  Workshop H2C ";
        d.model_id     = "O1C2";
        d.version      = "01.01.00.00";
        d.connect_type = "LAN";
        d.bind_state   = "Free";
        REQUIRE(vet_bind_detect_result(0, d) == 0);
        CHECK(d.dev_id == "31B00C000000001");
        CHECK(d.dev_name == "Workshop H2C");
        CHECK(d.model_id == "O1C2");
        CHECK(d.version == "01.01.00.00");
        CHECK(d.connect_type == "lan");
        CHECK(d.bind_state == "free");
    }
    SECTION("cloud / occupied are kept, so the dialog's own messages still fire")
    {
        detectResult d;
        d.dev_id       = "00M00A000000001";
        d.connect_type = "cloud";
        d.bind_state   = "OCCUPIED";
        REQUIRE(vet_bind_detect_result(0, d) == 0);
        CHECK(d.connect_type == "cloud");
        CHECK(d.bind_state == "occupied");
    }
}

TEST_CASE("bind_detect: what counts as a usable serial", "[BindDetectGuard]")
{
    CHECK(is_usable_serial("01P00A123456789"));
    CHECK(is_usable_serial("00M00A000000002"));
    CHECK_FALSE(is_usable_serial(""));
    CHECK_FALSE(is_usable_serial("SN_FAKE_0001"));
    CHECK_FALSE(is_usable_serial(std::string(65, 'A')));
}

TEST_CASE("bind worker: run_guarded turns an exception into a failure instead of terminating", "[BindDetectGuard]")
{
    using Slic3r::GUI::run_guarded;
    std::string seen;

    CHECK(run_guarded([] {}, [&](const std::string &w) { seen = w; }));
    CHECK(seen.empty());

    CHECK_FALSE(run_guarded([] { throw std::runtime_error("plug-in boom"); }, [&](const std::string &w) { seen = w; }));
    CHECK(seen == "plug-in boom");

    seen.clear();
    CHECK_FALSE(run_guarded([] { throw 42; }, [&](const std::string &w) { seen = w; }));
    CHECK(seen == "unknown exception");

    // an error handler that throws must not escape either
    CHECK_FALSE(run_guarded([] { throw std::runtime_error("x"); }, [](const std::string &) { throw std::runtime_error("y"); }));
}

TEST_CASE("bind worker: the alive token cuts off a worker's posts once the dialog is gone", "[BindDetectGuard]")
{
    using namespace Slic3r::GUI;
    CHECK_FALSE(ui_alive(AliveToken()));

    AliveToken token = make_alive_token();
    AliveToken worker_copy = token; // what the worker thread holds
    CHECK(ui_alive(worker_copy));

    int touched = 0;
    auto post = [&](const AliveToken &t) { if (ui_alive(t)) ++touched; };
    post(worker_copy);
    CHECK(touched == 1);

    kill_alive_token(token); // dialog cancelled / destroyed on the UI thread
    post(worker_copy);
    CHECK(touched == 1);
    kill_alive_token(AliveToken()); // null is fine
}
