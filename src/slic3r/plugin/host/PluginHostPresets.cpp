#include "PluginHostBindings.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "slic3r/plugin/OrcaApiVersion.hpp"

#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

py::list current_filament_presets(PresetBundle& bundle)
{
    py::list presets;
    for (const std::string& preset_name : bundle.filament_presets) {
        Preset* preset = bundle.filaments.find_preset(preset_name);
        if (preset == nullptr)
            presets.append(py::none());
        else
            presets.append(py::cast(preset, py::return_value_policy::reference));
    }
    return presets;
}

PresetCollection& printer_presets(PresetBundle& bundle)
{
    return static_cast<PresetCollection&>(bundle.printers);
}

struct PresetEditor
{
    Preset::Type type = Preset::TYPE_INVALID;
};

const char* preset_tab_name(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return "process";
    case Preset::TYPE_FILAMENT: return "filament";
    case Preset::TYPE_PRINTER:  return "printer";
    default:                    return "preset";
    }
}

[[noreturn]] void preset_write_unavailable(const char* name)
{
    throw edge_plugin_api_unavailable(std::string("orca.host.preset.") + name);
}

} // namespace

void host_bindings::register_presets(py::module_& host)
{
    auto preset  = host.def_submodule("preset", "Preset operations (read-only in this Edge build)");
    auto bundles = preset.def_submodule("bundles", "Local preset bundle operations");
    bundles.def(
        "reload_local",
        [](const std::string&) { preset_write_unavailable("bundles.reload_local"); },
        py::arg("bundle_id"),
        "Reload one local preset bundle. Not available in this Edge build.");
    bundles.def(
        "list_ids",
        []() -> std::vector<std::string> { preset_write_unavailable("bundles.list_ids"); },
        "IDs of local preset bundles. Not available in this Edge build.");

    py::enum_<Preset::Type>(host, "PresetType")
        .value("Invalid", Preset::TYPE_INVALID)
        .value("Print", Preset::TYPE_PRINT)
        .value("SlaPrint", Preset::TYPE_SLA_PRINT)
        .value("Filament", Preset::TYPE_FILAMENT)
        .value("SlaMaterial", Preset::TYPE_SLA_MATERIAL)
        .value("Printer", Preset::TYPE_PRINTER)
        .value("PhysicalPrinter", Preset::TYPE_PHYSICAL_PRINTER)
        .value("Plate", Preset::TYPE_PLATE)
        .value("Model", Preset::TYPE_MODEL);

    py::class_<PresetEditor>(preset, "PresetEditor",
                             "Handle to a category of currently edited preset. Writes are not available "
                             "in this Edge build (mutable presets are a later phase).")
        .def_readonly("type", &PresetEditor::type)
        .def(
            "begin_update",
            [](const PresetEditor&, const std::string&) { preset_write_unavailable("begin_update"); },
            py::arg("preset_name") = "",
            "Not available in this Edge build.")
        .def(
            "begin_update",
            [](const PresetEditor& editor, int, const std::string&) {
                if (editor.type != Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("The slot_number form of begin_update() is only available on "
                                                "orca.host.preset.filament");
                preset_write_unavailable("filament.begin_update");
            },
            py::arg("slot_number"), py::arg("preset_name") = "",
            "Not available in this Edge build.")
        .def(
            "update",
            [](const PresetEditor&, const std::string&, const std::string&) { preset_write_unavailable("update"); },
            py::arg("key"), py::arg("value"),
            "Not available in this Edge build.")
        .def(
            "update",
            [](const PresetEditor& editor, int, const std::string&, const std::string&) {
                if (editor.type != Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("The slot_number form of update() is only available on "
                                                "orca.host.preset.filament");
                preset_write_unavailable("filament.update");
            },
            py::arg("slot_number"), py::arg("key"), py::arg("value"),
            "Not available in this Edge build.");

    preset.attr("process")  = py::cast(PresetEditor{Preset::TYPE_PRINT});
    preset.attr("printer")  = py::cast(PresetEditor{Preset::TYPE_PRINTER});
    preset.attr("filament") = py::cast(PresetEditor{Preset::TYPE_FILAMENT});

    py::class_<Preset, std::unique_ptr<Preset, py::nodelete>>(host, "Preset")
        .def_readonly("type", &Preset::type)
        .def_readonly("name", &Preset::name)
        .def_readonly("alias", &Preset::alias)
        .def_readonly("file", &Preset::file)
        .def_readonly("is_default", &Preset::is_default)
        .def_readonly("is_external", &Preset::is_external)
        .def_readonly("is_system", &Preset::is_system)
        .def_readonly("is_visible", &Preset::is_visible)
        .def_readonly("is_dirty", &Preset::is_dirty)
        .def_readonly("is_compatible", &Preset::is_compatible)
        .def_readonly("is_project_embedded", &Preset::is_project_embedded)
        .def("is_user", &Preset::is_user)
        .def("label", &Preset::label, py::arg("no_alias") = false)
        .def("config_keys", [](const Preset& p) { return p.config.keys(); })
        .def("config_value", [](const Preset& p, const std::string& key) {
            return config_value_or_none(p.config, key);
        });

    py::class_<PresetCollection, std::unique_ptr<PresetCollection, py::nodelete>>(host, "PresetCollection")
        .def("size", &PresetCollection::size)
        .def("get_selected_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_selected_preset();
        }, py::return_value_policy::reference_internal)
        .def("selected_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_selected_preset();
        }, py::return_value_policy::reference_internal)
        .def("get_selected_preset_name", &PresetCollection::get_selected_preset_name)
        .def("selected_preset_name", &PresetCollection::get_selected_preset_name)
        .def("get_edited_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("edited_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("preset", [](PresetCollection& collection, size_t index) -> Preset& {
            if (index >= collection.size())
                throw py::index_error("preset index out of range");
            return collection.preset(index);
        }, py::return_value_policy::reference_internal)
        .def("find_preset", [](PresetCollection& collection, const std::string& name) -> Preset* {
            return collection.find_preset(name);
        }, py::return_value_policy::reference_internal)
        .def("preset_names", [](const PresetCollection& collection) {
            std::vector<std::string> names;
            names.reserve(collection.get_presets().size());
            for (const Preset& p : collection.get_presets())
                names.push_back(p.name);
            return names;
        });

    py::class_<PresetBundle, std::unique_ptr<PresetBundle, py::nodelete>>(host, "PresetBundle")
        .def_property_readonly("prints", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("printers", &printer_presets, py::return_value_policy::reference_internal)
        .def_property_readonly("filaments", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.filaments;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_prints", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.sla_prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_materials", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.sla_materials;
        }, py::return_value_policy::reference_internal)
        .def("current_process_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.prints.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_print_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.prints.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_printer_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.printers.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_filament_preset_names", [](PresetBundle& bundle) {
            return bundle.filament_presets;
        })
        .def("current_filament_presets", &current_filament_presets)
        .def("full_config_keys", [](const PresetBundle& bundle) {
            return bundle.full_config().keys();
        })
        .def("full_config_value", [](const PresetBundle& bundle, const std::string& key) {
            return config_value_or_none(bundle.full_config(), key);
        });

    (void) preset_tab_name;
}

} // namespace Slic3r
