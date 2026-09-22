#include <catch_main.hpp>

#include "slic3r/Utils/Http.hpp"

// These three cases talk to real hosts on the internet (github.com, jigsaw.w3.org), so their
// outcome says more about the network and those servers than about this code: jigsaw.w3.org's
// /HTTP/Basic/ answers 403 to every client now, curl included. They are hidden ([.network]) so a
// plain run and ctest skip them; run them on purpose with  slic3rutils_tests "[network]".

TEST_CASE("Check SSL certificates paths", "[Http][NotWorking][.network]") {
    
    Slic3r::Http g = Slic3r::Http::get("https://github.com/");
    
    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });
    
    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });
    
    g.perform_sync();
    
    REQUIRE(status == 200);
}

TEST_CASE("Http digest authentication", "[Http][NotWorking][.network]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Digest/");

    g.auth_digest("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}

TEST_CASE("Http basic authentication", "[Http][NotWorking][.network]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Basic/");

    g.auth_basic("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}

