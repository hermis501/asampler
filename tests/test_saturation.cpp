#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/Saturation.h"

#include <cmath>

using namespace asampler;

TEST_CASE ("Saturation amount 0 is a true bypass", "[saturation]")
{
    for (auto mode : { SaturationMode::Tube, SaturationMode::Tape, SaturationMode::HardFold })
    {
        REQUIRE (applySaturation (-0.7f, mode, 0.0f) == Catch::Approx (-0.7f));
        REQUIRE (applySaturation ( 0.0f, mode, 0.0f) == Catch::Approx ( 0.0f));
        REQUIRE (applySaturation ( 0.4f, mode, 0.0f) == Catch::Approx ( 0.4f));
    }
}

TEST_CASE ("Saturation maps zero to zero", "[saturation]")
{
    // Tape/HardFold are symmetric so an input of 0 must stay 0. Tube is
    // asymmetric but the curve still passes through the origin.
    REQUIRE (applySaturation (0.0f, SaturationMode::Tube,     1.0f) == Catch::Approx (0.0f).margin (1e-6));
    REQUIRE (applySaturation (0.0f, SaturationMode::Tape,     1.0f) == Catch::Approx (0.0f).margin (1e-6));
    REQUIRE (applySaturation (0.0f, SaturationMode::HardFold, 1.0f) == Catch::Approx (0.0f).margin (1e-6));
}

TEST_CASE ("Saturation preserves sign", "[saturation]")
{
    for (auto mode : { SaturationMode::Tube, SaturationMode::Tape, SaturationMode::HardFold })
    {
        REQUIRE (applySaturation ( 0.3f, mode, 0.6f) > 0.0f);
        REQUIRE (applySaturation (-0.3f, mode, 0.6f) < 0.0f);
    }
}

TEST_CASE ("Saturation output stays bounded for in-range audio", "[saturation]")
{
    // For real audio input (|x| <= 1) every curve keeps the output inside the
    // rails. (Soft curves can overshoot slightly past |x| = 1 input because the
    // tanh normalisation is referenced to the +/-1 rail — that's out of the
    // audio range and not a concern.)
    for (auto mode : { SaturationMode::Tube, SaturationMode::Tape, SaturationMode::HardFold })
    {
        for (float amt : { 0.25f, 0.5f, 1.0f })
        {
            REQUIRE (std::abs (applySaturation ( 1.0f, mode, amt)) <= 1.0001f);
            REQUIRE (std::abs (applySaturation (-1.0f, mode, amt)) <= 1.0001f);
            REQUIRE (std::isfinite (applySaturation ( 2.0f, mode, amt)));
        }
    }
}

TEST_CASE ("Saturation drive compresses the transfer curve", "[saturation]")
{
    // For a soft-clip curve, more drive pushes a mid-level input closer to the
    // rails: the output at high drive exceeds the output at low drive.
    const float lo = applySaturation (0.3f, SaturationMode::Tape, 0.1f);
    const float hi = applySaturation (0.3f, SaturationMode::Tape, 1.0f);
    REQUIRE (hi > lo);
}

TEST_CASE ("Saturation clamps out-of-range amount", "[saturation]")
{
    // amount above 1 behaves like amount == 1 (internal jlimit).
    const float atOne  = applySaturation (0.5f, SaturationMode::Tube, 1.0f);
    const float beyond = applySaturation (0.5f, SaturationMode::Tube, 5.0f);
    REQUIRE (beyond == Catch::Approx (atOne));
}

TEST_CASE ("Bit-depth reduction 16 or more is a bypass", "[saturation]")
{
    REQUIRE (applyBitDepthReduction (0.123f, 16) == Catch::Approx (0.123f));
    REQUIRE (applyBitDepthReduction (0.123f, 24) == Catch::Approx (0.123f));
    REQUIRE (applyBitDepthReduction (-0.9f,  16) == Catch::Approx (-0.9f));
}

TEST_CASE ("Bit-depth reduction quantises onto a coarse grid", "[saturation]")
{
    // 2 bits -> levels = 2^(2-1) = 2, step = 0.5. Each value snaps to the
    // nearest multiple of 0.5 via round(x/step)*step.
    REQUIRE (applyBitDepthReduction (0.30f, 2) == Catch::Approx (0.5f));  // round(0.60)=1
    REQUIRE (applyBitDepthReduction (0.24f, 2) == Catch::Approx (0.0f));  // round(0.48)=0
    REQUIRE (applyBitDepthReduction (0.80f, 2) == Catch::Approx (1.0f));  // round(1.60)=2

    // Exact grid points survive unchanged.
    REQUIRE (applyBitDepthReduction (0.5f, 2) == Catch::Approx (0.5f));
    REQUIRE (applyBitDepthReduction (0.0f, 2) == Catch::Approx (0.0f));
}

TEST_CASE ("Bit-depth reduction clamps absurd bit counts", "[saturation]")
{
    // bits below 2 clamps to 2 (step 0.5); this must not divide-by-zero or blow up.
    const float clamped = applyBitDepthReduction (0.30f, -3);
    REQUIRE (clamped == Catch::Approx (0.5f));
    REQUIRE (std::isfinite (clamped));
}
