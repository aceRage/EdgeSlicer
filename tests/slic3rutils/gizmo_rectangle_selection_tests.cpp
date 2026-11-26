#include <catch2/catch.hpp>

#include "slic3r/GUI/Gizmos/GizmoRectangleSelection.hpp"

using namespace Slic3r::GUI;

// Stand-in for GLGizmosManager. Numeric values do not need to match: the helper
// compares by enumerator name. Edge has no PaintedModifier (OrcaSlicer #17533).
struct FakeGizmos
{
    enum EType : unsigned char
    {
        Move,
        Rotate,
        Scale,
        Flatten,
        Cut,
        MeshBoolean,
        FdmSupports,
        Seam,
        FuzzySkin,
        MmSegmentation,
        TextureDisplacement,
        Emboss,
        Svg,
        Measure,
        Assembly,
        Simplify,
        BrimEars,
        Sculpt,
        Edit,
        CadFillet,
        SplitToFit,
        Undefined
    };
};

TEST_CASE("Measure and Assembly do not start a rectangle selection on Shift-drag", "[GizmoRectangleSelection]")
{
    const auto type = GENERATE(FakeGizmos::Measure, FakeGizmos::Assembly);
    REQUIRE_FALSE(gizmo_allows_shift_rectangle_selection<FakeGizmos>(type));
}

TEST_CASE("Painter gizmos and Cut still suppress Shift-drag rectangle selection", "[GizmoRectangleSelection]")
{
    const auto type = GENERATE(FakeGizmos::FdmSupports, FakeGizmos::Seam, FakeGizmos::Cut,
        FakeGizmos::MmSegmentation, FakeGizmos::FuzzySkin);
    REQUIRE_FALSE(gizmo_allows_shift_rectangle_selection<FakeGizmos>(type));
}

TEST_CASE("Shift-drag rectangle selection remains available with no gizmo and with Move", "[GizmoRectangleSelection]")
{
    const auto type = GENERATE(FakeGizmos::Undefined, FakeGizmos::Move, FakeGizmos::Rotate,
        FakeGizmos::Scale, FakeGizmos::Flatten, FakeGizmos::MeshBoolean, FakeGizmos::TextureDisplacement,
        FakeGizmos::Emboss, FakeGizmos::Svg, FakeGizmos::Simplify, FakeGizmos::BrimEars,
        FakeGizmos::Sculpt, FakeGizmos::Edit, FakeGizmos::CadFillet, FakeGizmos::SplitToFit);
    REQUIRE(gizmo_allows_shift_rectangle_selection<FakeGizmos>(type));
}
