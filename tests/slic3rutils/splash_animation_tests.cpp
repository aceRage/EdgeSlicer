#include <catch2/catch.hpp>

#include "slic3r/GUI/SplashAnimation.hpp"

#include <cmath>
#include <vector>

using namespace Slic3r::GUI::SplashAnim;

// The startup splash draws a small object being built up layer by layer behind the logo. The
// drawing itself needs a wxGraphicsContext and a live window, so it is not exercised here; what is
// exercised is the geometry and the timeline in SplashAnimation.hpp, which is what actually decides
// whether the animation reads as an object being sliced.
//
// These numbers are also the contract the preview script draws from, so a change to the shape that
// nobody meant to make shows up here rather than only in the GIF.

SCENARIO("the splash silhouette is a sensible printable shape", "[Splash]")
{
    GIVEN("the object profile")
    {
        THEN("it is solid at every height: no zero-width slice to print")
        {
            for (int i = 0; i <= 200; ++i) {
                const float t  = float(i) / 200.0f;
                const float hw = half_width_at(t);
                INFO("t = " << t);
                REQUIRE(hw > 1.0f);
            }
        }

        THEN("it never exceeds the bounding box it was given")
        {
            const float max_half = (k_obj_right - k_obj_left) * 0.5f;
            for (int i = 0; i <= 200; ++i) {
                const float t = float(i) / 200.0f;
                INFO("t = " << t);
                REQUIRE(half_width_at(t) <= max_half + 1e-4f);
            }
        }

        THEN("the hull widens from the keel up to the deck")
        {
            // Monotonic over the hull, so the lower half reads as a boat rather than a blob.
            float prev = half_width_at(0.0f);
            for (int i = 1; i <= 50; ++i) {
                const float t  = float(i) / 100.0f;
                const float hw = half_width_at(t);
                INFO("t = " << t);
                REQUIRE(hw >= prev - 1e-4f);
                prev = hw;
            }
        }

        THEN("the cabin is a clear step in from the deck")
        {
            // The silhouette has to have a visible shoulder, or the cabin is invisible and the
            // object stops reading as a boat and starts reading as a pot.
            const float deck  = half_width_at(0.60f);
            const float cabin = half_width_at(0.63f);
            REQUIRE(cabin < deck * 0.6f);
        }

        THEN("the cabin tapers towards the roof but stays solid")
        {
            REQUIRE(half_width_at(1.0f) < half_width_at(0.63f));
            REQUIRE(half_width_at(1.0f) > 1.0f);
        }

        THEN("it is wider than it is tall, the way a boat is")
        {
            // A tall narrow silhouette reads as a bottle; the object has to be a squat one.
            REQUIRE((k_obj_right - k_obj_left) > (k_obj_bottom - k_obj_top));
        }
    }
}

SCENARIO("the splash animation timeline builds then holds", "[Splash]")
{
    GIVEN("the loop")
    {
        THEN("it starts empty and finishes built")
        {
            REQUIRE(build_at(0.0f) == Approx(0.0f));
            REQUIRE(build_at(k_build_end) == Approx(1.0f));
        }

        THEN("the build only ever advances")
        {
            float prev = 0.0f;
            for (int i = 0; i <= 100; ++i) {
                const float b = build_at(float(i) / 100.0f);
                INFO("phase = " << float(i) / 100.0f);
                REQUIRE(b >= prev - 1e-6f);
                prev = b;
            }
        }

        THEN("the tail of the loop holds the finished object rather than overshooting")
        {
            for (int i = 0; i <= 10; ++i) {
                const float phase = k_build_end + (1.0f - k_build_end) * float(i) / 10.0f;
                INFO("phase = " << phase);
                REQUIRE(build_at(phase) == Approx(1.0f));
            }
        }

        THEN("the loop is 2.5 s, as the splash was asked for")
        {
            REQUIRE(k_loop_ms == 2500);
        }

        THEN("there are enough layers to read as layers, and few enough to stay calm")
        {
            REQUIRE(layer_count() >= 20);
            REQUIRE(layer_count() <= 48);
        }
    }
}

SCENARIO("the splash animation stays inside its card", "[Splash]")
{
    GIVEN("the design space and the object box")
    {
        THEN("the object sits within the card with room for the plate line")
        {
            REQUIRE(k_obj_left > 0.0f);
            REQUIRE(k_obj_right < float(k_design_w));
            REQUIRE(k_obj_top > 0.0f);
            REQUIRE(k_obj_bottom + 2.0f < float(k_design_h));
        }

        THEN("the slicing head stays between the plate and the top of the object")
        {
            const float obj_h = k_obj_bottom - k_obj_top;
            for (int i = 0; i <= 100; ++i) {
                const float phase  = float(i) / 100.0f;
                const float head_y = k_obj_bottom - obj_h * build_at(phase);
                INFO("phase = " << phase);
                REQUIRE(head_y <= k_obj_bottom + 1e-4f);
                REQUIRE(head_y >= k_obj_top - 1e-4f);
            }
        }
    }
}
