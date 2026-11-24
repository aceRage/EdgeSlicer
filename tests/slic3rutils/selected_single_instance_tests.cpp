#include <catch2/catch.hpp>

#include "slic3r/GUI/SelectedSingleInstance.hpp"

#include <vector>

using namespace Slic3r::GUI;

// OrcaSlicer #17642: Selection::get_selected_single_intance() indexed
// ModelObject::instances[get_instance_idx()] with no check. get_instance_idx() is -1 when more
// than one instance is selected (Object coordinates + Ctrl+A, or two copies). The helper is
// header-only so the index rule can be tested without constructing a GUI Selection. Stand-in
// pointers fill in for ModelInstance*: the template only needs a random-access list of pointers.

namespace {

struct FakeInstance {};

} // namespace

TEST_CASE("A multi-instance selection gives the Move gizmo no instance", "[SelectedSingleInstance]")
{
    FakeInstance a, b;
    const std::vector<FakeInstance *> instances{&a, &b};

    // -1 is what Selection::get_instance_idx() returns for a multi-instance selection.
    REQUIRE(selected_single_instance(instances, -1) == nullptr);
    REQUIRE(selected_single_instance(instances, -2) == nullptr);
}

TEST_CASE("An index past the end of the instance list gives the Move gizmo no instance", "[SelectedSingleInstance]")
{
    FakeInstance a, b;
    const std::vector<FakeInstance *> instances{&a, &b};

    REQUIRE(selected_single_instance(instances, 2) == nullptr);
    REQUIRE(selected_single_instance(instances, 100) == nullptr);

    const std::vector<FakeInstance *> empty;
    REQUIRE(selected_single_instance(empty, 0) == nullptr);
}

TEST_CASE("A single-instance selection gives the Move gizmo that instance", "[SelectedSingleInstance]")
{
    FakeInstance first, second;
    const std::vector<FakeInstance *> instances{&first, &second};

    REQUIRE(selected_single_instance(instances, 0) == &first);
    REQUIRE(selected_single_instance(instances, 1) == &second);
}
