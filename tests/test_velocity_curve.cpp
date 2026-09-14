#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/VelocityCurve.h"

#include <cmath>

using namespace asampler;

TEST_CASE ("Velocity curve Fixed ignores velocity", "[velocity]")
{
    REQUIRE (applyVelocityCurve (0.0f, VelocityCurve::Fixed) == Catch::Approx (1.0f));
    REQUIRE (applyVelocityCurve (0.5f, VelocityCurve::Fixed) == Catch::Approx (1.0f));
    REQUIRE (applyVelocityCurve (1.0f, VelocityCurve::Fixed) == Catch::Approx (1.0f));
}

TEST_CASE ("Velocity curve Linear is identity", "[velocity]")
{
    REQUIRE (applyVelocityCurve (0.00f, VelocityCurve::Linear) == Catch::Approx (0.00f));
    REQUIRE (applyVelocityCurve (0.25f, VelocityCurve::Linear) == Catch::Approx (0.25f));
    REQUIRE (applyVelocityCurve (0.75f, VelocityCurve::Linear) == Catch::Approx (0.75f));
    REQUIRE (applyVelocityCurve (1.00f, VelocityCurve::Linear) == Catch::Approx (1.00f));
}

TEST_CASE ("Velocity curve Soft squares the input", "[velocity]")
{
    REQUIRE (applyVelocityCurve (0.5f, VelocityCurve::Soft) == Catch::Approx (0.25f));
    REQUIRE (applyVelocityCurve (0.8f, VelocityCurve::Soft) == Catch::Approx (0.64f));
    // Endpoints unchanged.
    REQUIRE (applyVelocityCurve (0.0f, VelocityCurve::Soft) == Catch::Approx (0.0f));
    REQUIRE (applyVelocityCurve (1.0f, VelocityCurve::Soft) == Catch::Approx (1.0f));
}

TEST_CASE ("Velocity curve Hard takes the square root", "[velocity]")
{
    REQUIRE (applyVelocityCurve (0.25f, VelocityCurve::Hard) == Catch::Approx (0.5f));
    REQUIRE (applyVelocityCurve (0.64f, VelocityCurve::Hard) == Catch::Approx (0.8f));
    REQUIRE (applyVelocityCurve (0.0f,  VelocityCurve::Hard) == Catch::Approx (0.0f));
    REQUIRE (applyVelocityCurve (1.0f,  VelocityCurve::Hard) == Catch::Approx (1.0f));
}

TEST_CASE ("Velocity curve clamps out-of-range input", "[velocity]")
{
    // Below zero collapses to zero across every curve.
    REQUIRE (applyVelocityCurve (-1.0f, VelocityCurve::Linear) == Catch::Approx (0.0f));
    REQUIRE (applyVelocityCurve (-1.0f, VelocityCurve::Soft)   == Catch::Approx (0.0f));
    REQUIRE (applyVelocityCurve (-1.0f, VelocityCurve::Hard)   == Catch::Approx (0.0f));

    // Above one collapses to one.
    REQUIRE (applyVelocityCurve (2.0f, VelocityCurve::Linear) == Catch::Approx (1.0f));
    REQUIRE (applyVelocityCurve (2.0f, VelocityCurve::Soft)   == Catch::Approx (1.0f));
    REQUIRE (applyVelocityCurve (2.0f, VelocityCurve::Hard)   == Catch::Approx (1.0f));
}
