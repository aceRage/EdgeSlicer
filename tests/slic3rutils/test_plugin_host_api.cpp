#include <catch2/catch.hpp>

#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>
#include <slic3r/plugin/OrcaApiVersion.hpp>
#include <slic3r/plugin/host/PluginHostBindings.hpp>

#include <pybind11/cast.h>
#include <pybind11/embed.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <string>

namespace py = pybind11;
using namespace Slic3r;

namespace {

struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-python"};
    bool          initialized = PluginManager::instance().initialize();
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

bool has_attr(const py::handle& object, const char* name) { return py::hasattr(object, name); }

py::module_ import_orca_module()
{
    (void) PythonPluginBridge::instance();
    return py::module_::import("orca");
}

} // namespace

TEST_CASE("Plugin host API exposes host-owned bundle and preset surface to Python", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    py::module_ orca = import_orca_module();
    REQUIRE(has_attr(orca, "host"));
    CHECK(orca.attr("api_version").cast<std::string>() == k_orca_api_version);

    py::object host = orca.attr("host");
    REQUIRE(has_attr(host, "PresetBundle"));
    REQUIRE(has_attr(host, "Preset"));
    REQUIRE(has_attr(host, "PresetCollection"));
    REQUIRE(has_attr(host, "Model"));
    REQUIRE(has_attr(host, "ModelObject"));
    REQUIRE(has_attr(host, "Plater"));
    REQUIRE(has_attr(host, "Print"));
    REQUIRE(has_attr(host, "PrintObject"));
    REQUIRE(has_attr(host, "Layer"));
    REQUIRE(has_attr(host, "LayerRegion"));
    REQUIRE(has_attr(host, "Surface"));
    REQUIRE(has_attr(host, "preset"));
    py::object preset = host.attr("preset");
    REQUIRE(has_attr(preset, "bundles"));
    py::object bundles = preset.attr("bundles");
    CHECK(has_attr(bundles, "list_ids"));
    CHECK(has_attr(bundles, "reload_local"));
    CHECK_FALSE(has_attr(host, "reload_local_bundle"));
    CHECK_FALSE(has_attr(host, "list_bundle_ids"));

    py::object preset_bundle_type = host.attr("PresetBundle");
    CHECK(has_attr(preset_bundle_type, "prints"));
    CHECK(has_attr(preset_bundle_type, "printers"));
    CHECK(has_attr(preset_bundle_type, "filaments"));
    CHECK(has_attr(preset_bundle_type, "current_process_preset"));
    CHECK(has_attr(preset_bundle_type, "current_printer_preset"));
    CHECK(has_attr(preset_bundle_type, "current_filament_preset_names"));
    CHECK(has_attr(preset_bundle_type, "current_filament_presets"));
    CHECK(has_attr(preset_bundle_type, "full_config_value"));

    py::object preset_collection_type = host.attr("PresetCollection");
    CHECK(has_attr(preset_collection_type, "get_edited_preset"));
    CHECK(has_attr(preset_collection_type, "get_selected_preset"));
    CHECK(has_attr(preset_collection_type, "get_selected_preset_name"));
    CHECK(has_attr(preset_collection_type, "edited_preset"));
    CHECK(has_attr(preset_collection_type, "selected_preset"));
    CHECK(has_attr(preset_collection_type, "selected_preset_name"));

    py::object preset_type = host.attr("Preset");
    CHECK(has_attr(preset_type, "name"));
    CHECK(has_attr(preset_type, "type"));
    CHECK(has_attr(preset_type, "is_default"));
    CHECK(has_attr(preset_type, "is_system"));
    CHECK(has_attr(preset_type, "is_user"));
    CHECK(has_attr(preset_type, "config_value"));

    PresetBundle bundle;
    Preset&      printer_preset  = bundle.printers.get_edited_preset();
    Preset&      process_preset  = bundle.prints.get_edited_preset();
    Preset&      filament_preset = bundle.filaments.get_edited_preset();

    printer_preset.config.set("printer_model", "Plugin Host Test Printer", true);
    bundle.filament_presets = {filament_preset.name, filament_preset.name, "missing filament preset"};

    py::object py_bundle = py::cast(&bundle, py::return_value_policy::reference);

    CHECK(py_bundle.attr("current_printer_preset")().attr("name").cast<std::string>() == printer_preset.name);
    CHECK(py_bundle.attr("current_print_preset")().attr("name").cast<std::string>() == process_preset.name);
    CHECK(py_bundle.attr("current_process_preset")().attr("name").cast<std::string>() == process_preset.name);
    CHECK(py_bundle.attr("current_printer_preset")().attr("is_default").cast<bool>() == printer_preset.is_default);
    CHECK(py_bundle.attr("current_printer_preset")().attr("is_user")().cast<bool>() == printer_preset.is_user());
    CHECK(py_bundle.attr("current_printer_preset")().attr("config_value")("printer_model").cast<std::string>() ==
          "Plugin Host Test Printer");
    CHECK(py_bundle.attr("current_printer_preset")().attr("config_value")("missing_test_key").is_none());

    py::list filament_names = py_bundle.attr("current_filament_preset_names")();
    REQUIRE(py::len(filament_names) == 3);
    CHECK(filament_names[0].cast<std::string>() == filament_preset.name);
    CHECK(filament_names[2].cast<std::string>() == "missing filament preset");

    py::list filament_presets = py_bundle.attr("current_filament_presets")();
    REQUIRE(py::len(filament_presets) == 3);
    CHECK_FALSE(filament_presets[0].is_none());
    CHECK(filament_presets[2].is_none());
}

TEST_CASE("Plugin host API preset writes are not available in this Edge build", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    py::object host   = import_orca_module().attr("host");
    py::object preset = host.attr("preset");
    REQUIRE(has_attr(preset, "PresetEditor"));
    REQUIRE(has_attr(preset, "process"));
    REQUIRE(has_attr(preset, "printer"));
    REQUIRE(has_attr(preset, "filament"));

    py::object process  = preset.attr("process");
    py::object printer  = preset.attr("printer");
    py::object filament = preset.attr("filament");
    CHECK(process.attr("type").cast<Preset::Type>() == Preset::TYPE_PRINT);
    CHECK(printer.attr("type").cast<Preset::Type>() == Preset::TYPE_PRINTER);
    CHECK(filament.attr("type").cast<Preset::Type>() == Preset::TYPE_FILAMENT);

    auto expect_unavailable = [](auto&& call) {
        try {
            call();
            FAIL("preset write unexpectedly succeeded");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_RuntimeError));
            CHECK(std::string(error.what()).find("not available in this Edge build") != std::string::npos);
        }
    };

    expect_unavailable([&] { process.attr("update")("layer_height", "0.25"); });
    expect_unavailable([&] { printer.attr("begin_update")(); });
    expect_unavailable([&] { filament.attr("begin_update")(1); });
    expect_unavailable([&] { filament.attr("update")(1, "layer_height", "0.25"); });
    expect_unavailable([&] { preset.attr("bundles").attr("reload_local")("test-bundle"); });
    expect_unavailable([&] { preset.attr("bundles").attr("list_ids")(); });

    for (const char* name : {"process", "printer"}) {
        try {
            preset.attr(name).attr("update")(1, "layer_height", "0.25");
            FAIL("slot-addressed update on a non-filament editor unexpectedly succeeded");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_ValueError));
            CHECK(std::string(error.what()).find("only available on") != std::string::npos);
        }
    }
}

TEST_CASE("Plugin host API reports unavailable GUI objects before Orca app initialization", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    py::object host = import_orca_module().attr("host");
    for (const char* function_name : {"preset_bundle", "plater", "model", "app_info", "selected_printer"}) {
        CAPTURE(function_name);
        try {
            host.attr(function_name)();
            FAIL("host accessor unexpectedly succeeded without a wx application");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_RuntimeError));
            CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
        }
    }

    CHECK(host.attr("gl_info")().is_none());
    py::object plater_type = host.attr("Plater");
    CHECK(has_attr(plater_type, "project_path"));
    CHECK(has_attr(plater_type, "export_3mf_copy"));
}

TEST_CASE("Plugin host API exposes the UI module and guards it before Orca app initialization", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    py::object host = import_orca_module().attr("host");
    REQUIRE(has_attr(host, "ui"));
    py::object ui = host.attr("ui");
    CHECK(has_attr(ui, "message"));
    CHECK_FALSE(has_attr(ui, "show_dialog"));
    CHECK(has_attr(ui, "create_window"));
    CHECK(has_attr(ui, "WINDOW_MODELESS"));
    CHECK(has_attr(ui, "WINDOW_MODAL"));
    CHECK(ui.attr("WINDOW_MODELESS").cast<long>() == 0);
    CHECK(ui.attr("WINDOW_MODAL").cast<long>() == 1);
    CHECK(has_attr(ui, "UiWindow"));
    CHECK(has_attr(ui, "create_dock_panel"));
    CHECK(has_attr(ui, "UiDockPanel"));
    CHECK(has_attr(ui, "push_notification"));
    CHECK(has_attr(ui, "create_progress_dialog"));

    try {
        ui.attr("message")("hello");
        FAIL("orca.host.ui.message unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }

    try {
        ui.attr("create_window")("<p>hi</p>");
        FAIL("orca.host.ui.create_window unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }

    try {
        ui.attr("create_dock_panel")("<p>panel</p>", "OrcaSlicer", 320, 480, py::none(), py::none(), "top");
        FAIL("orca.host.ui.create_dock_panel accepted an unknown dock position");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_ValueError));
    }

    try {
        ui.attr("create_dock_panel")("<p>panel</p>");
        FAIL("orca.host.ui.create_dock_panel unexpectedly succeeded");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("not available in this Edge build") != std::string::npos);
    }
}

TEST_CASE("Plugin host API exposes model geometry and structure to Python", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    py::object host = import_orca_module().attr("host");
    REQUIRE(has_attr(host, "BoundingBox"));
    REQUIRE(has_attr(host, "Model"));
    REQUIRE(has_attr(host, "ModelInstance"));
    REQUIRE(has_attr(host, "ModelVolume"));
    REQUIRE(has_attr(host, "ModelVolumeType"));

    Model        model;
    ModelObject* object = model.add_object();
    object->name        = "Plugin Host Test Cube";
    ModelVolume* part   = object->add_volume(make_cube(10.0, 20.0, 30.0));
    part->name          = "cube part";
    ModelVolume* modifier =
        object->add_volume(make_cube(2.0, 2.0, 2.0), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->name = "fit modifier";
    ModelInstance* instance = object->add_instance();
    instance->set_offset(Vec3d(5.0, 6.0, 0.0));

    py::object py_model = py::cast(&model, py::return_value_policy::reference);
    CHECK(py_model.attr("object_count")().cast<size_t>() == 1);
    py::object py_object = py_model.attr("object")(0);
    CHECK(py_object.attr("name").cast<std::string>() == "Plugin Host Test Cube");
    CHECK(py_object.attr("instance_count")().cast<size_t>() == 1);
    CHECK(py_object.attr("volume_count")().cast<size_t>() == 2);
    CHECK(py_object.attr("is_multiparts")().cast<bool>());

    py::object py_instance = py_object.attr("instance")(0);
    py::object inst_offset = py_instance.attr("offset")();
    CHECK(inst_offset[py::int_(0)].cast<double>() == Approx(5.0));
    CHECK(inst_offset[py::int_(1)].cast<double>() == Approx(6.0));

    py::object py_part = py_object.attr("volume")(0);
    CHECK(py_part.attr("name").cast<std::string>() == "cube part");
    CHECK(py_part.attr("is_model_part")().cast<bool>());
    CHECK(py_part.attr("type")().cast<ModelVolumeType>() == ModelVolumeType::MODEL_PART);
    CHECK(py_part.attr("facets_count")().cast<size_t>() == 12);

    py::object py_modifier = py_object.attr("volume")(1);
    CHECK(py_modifier.attr("is_modifier")().cast<bool>());
}

TEST_CASE("Plugin host model child accessors keep the snapshot alive after GC", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    Model        live;
    ModelObject* object = live.add_object();
    object->name        = "kept-alive";
    ModelVolume* part   = object->add_volume(make_cube(10.0, 20.0, 30.0));
    part->name          = "kept-volume";
    object->add_instance();
    host_bindings::set_live_model_for_testing(&live);
    struct LiveModelGuard
    {
        ~LiveModelGuard() { host_bindings::set_live_model_for_testing(nullptr); }
    } live_guard;

    py::dict locals;
    py::exec(R"PY(
import gc
import orca
o = orca.host.model().objects()[0]
v = o.volumes()[0]
gc.collect()
name = o.name
vname = v.name
facets = v.facets_count()
fresh = orca.host.model()
fresh_name = fresh.objects()[0].name
fresh_vname = fresh.objects()[0].volumes()[0].name
fresh_facets = fresh.objects()[0].volumes()[0].facets_count()
)PY",
             py::globals(), locals);

    CHECK(locals["name"].cast<std::string>() == "kept-alive");
    CHECK(locals["vname"].cast<std::string>() == "kept-volume");
    CHECK(locals["facets"].cast<size_t>() == 12);
    CHECK(locals["fresh_name"].cast<std::string>() == "kept-alive");
    CHECK(locals["fresh_vname"].cast<std::string>() == "kept-volume");
    CHECK(locals["fresh_facets"].cast<size_t>() == 12);
}

TEST_CASE("preset_bundle snapshot sees a second edit while already dirty", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    PresetBundle live;
    live.printers.get_edited_preset().config.set("printer_model", "edit-one", true);
    live.printers.get_edited_preset().set_dirty(true);
    host_bindings::set_live_preset_bundle_for_testing(&live);
    struct LiveBundleGuard
    {
        ~LiveBundleGuard() { host_bindings::set_live_preset_bundle_for_testing(nullptr); }
    } live_guard;

    py::module_ host = import_orca_module().attr("host");
    auto        read_model = [&]() {
        return host.attr("preset_bundle")()
            .attr("current_printer_preset")()
            .attr("config_value")("printer_model")
            .cast<std::string>();
    };
    CHECK(read_model() == "edit-one");

    live.printers.get_edited_preset().config.set("printer_model", "edit-two", true);
    live.printers.get_edited_preset().set_dirty(true);
    CHECK(read_model() == "edit-two");
}

TEST_CASE("preset_bundle snapshot sees an edit-then-save with no read in between", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    PresetBundle live;
    live.printers.get_edited_preset().config.set("printer_model", "before-save", true);
    live.printers.get_edited_preset().set_dirty(true);
    host_bindings::set_live_preset_bundle_for_testing(&live);
    struct LiveBundleGuard
    {
        ~LiveBundleGuard() { host_bindings::set_live_preset_bundle_for_testing(nullptr); }
    } live_guard;

    py::module_ host = import_orca_module().attr("host");
    auto        read_model = [&]() {
        return host.attr("preset_bundle")()
            .attr("current_printer_preset")()
            .attr("config_value")("printer_model")
            .cast<std::string>();
    };
    CHECK(read_model() == "before-save");

    live.printers.get_edited_preset().config.set("printer_model", "after-save", true);
    live.printers.get_edited_preset().set_dirty(false);
    CHECK(read_model() == "after-save");
}

TEST_CASE("Plugin host API exposes TriangleMesh geometry to Python", "[PluginHost][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    py::object host = import_orca_module().attr("host");
    REQUIRE(has_attr(host, "TriangleMesh"));

    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10.0, 20.0, 30.0));
    object->add_instance();

    py::object py_object = py::cast(object, py::return_value_policy::reference);
    py::object mesh      = py_object.attr("volume")(0).attr("mesh")();
    CHECK(mesh.attr("vertex_count")().cast<size_t>() == 8);
    CHECK(mesh.attr("triangle_count")().cast<size_t>() == 12);
    CHECK_FALSE(mesh.attr("is_empty")().cast<bool>());
    CHECK(mesh.attr("volume")().cast<double>() == Approx(6000.0).epsilon(0.01));
    CHECK_THROWS_AS(mesh.attr("vertex")(8), py::error_already_set);
}
