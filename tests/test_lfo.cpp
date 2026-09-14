#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/Lfo.h"

#include <cmath>

using namespace asampler;

TEST_CASE ("LFO shapes stay within [-1, 1]", "[lfo]")
{
    for (auto shape : { LfoShape::Sine, LfoShape::Triangle, LfoShape::Square, LfoShape::Saw })
    {
        for (int i = 0; i < 100; ++i)
        {
            const double phase = i / 100.0;
            const float v = lfoValue (shape, phase);
            REQUIRE (v >= -1.0001f);
            REQUIRE (v <=  1.0001f);
        }
    }
}

TEST_CASE ("LFO wraps phase into one period", "[lfo]")
{
    for (auto shape : { LfoShape::Sine, LfoShape::Triangle, LfoShape::Square, LfoShape::Saw })
    {
        REQUIRE (lfoValue (shape, 0.3)  == Catch::Approx (lfoValue (shape, 1.3)).margin (1e-6));
        REQUIRE (lfoValue (shape, 0.3)  == Catch::Approx (lfoValue (shape, 2.3)).margin (1e-6));
        // Negative phases wrap too.
        REQUIRE (lfoValue (shape, 0.75) == Catch::Approx (lfoValue (shape, -0.25)).margin (1e-6));
    }
}

TEST_CASE ("Sine LFO hits its cardinal points", "[lfo]")
{
    REQUIRE (lfoValue (LfoShape::Sine, 0.0)  == Catch::Approx (0.0).margin (1e-6));
    REQUIRE (lfoValue (LfoShape::Sine, 0.25) == Catch::Approx (1.0).margin (1e-6));
    REQUIRE (lfoValue (LfoShape::Sine, 0.5)  == Catch::Approx (0.0).margin (1e-6));
    REQUIRE (lfoValue (LfoShape::Sine, 0.75) == Catch::Approx (-1.0).margin (1e-6));
}

TEST_CASE ("Triangle LFO is a symmetric 0 -> +1 -> -1 -> 0 ramp", "[lfo]")
{
    REQUIRE (lfoValue (LfoShape::Triangle, 0.0)   == Catch::Approx (0.0));
    REQUIRE (lfoValue (LfoShape::Triangle, 0.25)  == Catch::Approx (1.0));
    REQUIRE (lfoValue (LfoShape::Triangle, 0.5)   == Catch::Approx (0.0).margin (1e-6));
    REQUIRE (lfoValue (LfoShape::Triangle, 0.75)  == Catch::Approx (-1.0));
    REQUIRE (lfoValue (LfoShape::Triangle, 0.125) == Catch::Approx (0.5));
}

TEST_CASE ("Square LFO is a two-level step", "[lfo]")
{
    REQUIRE (lfoValue (LfoShape::Square, 0.0)  == Catch::Approx (1.0));
    REQUIRE (lfoValue (LfoShape::Square, 0.49) == Catch::Approx (1.0));
    REQUIRE (lfoValue (LfoShape::Square, 0.5)  == Catch::Approx (-1.0));
    REQUIRE (lfoValue (LfoShape::Square, 0.99) == Catch::Approx (-1.0));
}

TEST_CASE ("Saw LFO is a rising ramp from -1 to +1", "[lfo]")
{
    REQUIRE (lfoValue (LfoShape::Saw, 0.0)  == Catch::Approx (-1.0));
    REQUIRE (lfoValue (LfoShape::Saw, 0.5)  == Catch::Approx (0.0).margin (1e-6));
    REQUIRE (lfoValue (LfoShape::Saw, 0.75) == Catch::Approx (0.5));
}
