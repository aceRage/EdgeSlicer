#include "SlicingPipelinePluginCapability.hpp"
#include "SlicingPipelinePluginCapabilityTrampoline.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/cast.h>
#include <pybind11/pytypes.h>
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include <string>
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include <memory>

namespace py = pybind11;
namespace Slic3r {

bool SlicingPipelineContext::cancelled() const { return print && print->canceled(); }

void SlicingPipelinePluginCapability::RegisterBindings(py::module_& module) {
    auto slicing = module.def_submodule("slicing", "Slicing pipeline API.");

    py::enum_<SlicingPipelineStepPlugin>(slicing, "Step")
        .value("posSlice", SlicingPipelineStepPlugin::posSlice)
        .value("posPerimeters", SlicingPipelineStepPlugin::posPerimeters)
        .value("posEstimateCurledExtrusions", SlicingPipelineStepPlugin::posEstimateCurledExtrusions)
        .value("posPrepareInfill", SlicingPipelineStepPlugin::posPrepareInfill)
        .value("posInfill", SlicingPipelineStepPlugin::posInfill)
        .value("posIroning", SlicingPipelineStepPlugin::posIroning)
        .value("posContouring", SlicingPipelineStepPlugin::posContouring)
        .value("posSupportMaterial", SlicingPipelineStepPlugin::posSupportMaterial)
        .value("posDetectOverhangsForLift", SlicingPipelineStepPlugin::posDetectOverhangsForLift)
        .value("posSimplifyPath", SlicingPipelineStepPlugin::posSimplifyPath)
        .value("psWipeTower", SlicingPipelineStepPlugin::psWipeTower)
        .value("psSkirtBrim", SlicingPipelineStepPlugin::psSkirtBrim)
        .value("psGCodePostProcess", SlicingPipelineStepPlugin::psGCodePostProcess)
        .export_values();

    slicing.def("unscale", [](coord_t v) { return unscale<double>(v); }, py::arg("coord"),
        "Convert a scaled integer coordinate to millimeters (reads the live SCALING_FACTOR).");

    py::class_<SlicingPipelineContext>(slicing, "SlicingPipelineContext")
        .def_readonly("orca_version", &SlicingPipelineContext::orca_version)
        .def_readonly("step", &SlicingPipelineContext::step)
        .def_readonly("gcode_path", &SlicingPipelineContext::gcode_path,
            "Path to the working G-code file, set ONLY at Step.psGCodePostProcess. Edit it in "
            "place; empty at every other step.")
        .def_readonly("host", &SlicingPipelineContext::host,
            "Target host at Step.psGCodePostProcess (\"File\", \"OctoPrint\", ...); empty otherwise.")
        .def_readonly("output_name", &SlicingPipelineContext::output_name,
            "Final output G-code name at Step.psGCodePostProcess (mirrors SLIC3R_PP_OUTPUT_NAME); "
            "empty otherwise.")
        .def_property_readonly("print", [](const SlicingPipelineContext& ctx) -> py::object {
            if (ctx.print == nullptr)
                return py::none();
            return py::cast(ctx.print, py::return_value_policy::reference);
        }, "The orca.host.Print being sliced — the raw slicing graph. Valid only during execute(ctx). "
           "Early steps run unsandboxed in-process.")
        .def_property_readonly("object", [](const SlicingPipelineContext& ctx) -> py::object {
            if (ctx.object == nullptr)
                return py::none();
            return py::cast(const_cast<PrintObject*>(ctx.object), py::return_value_policy::reference);
        }, "orca.host.PrintObject for object-scoped steps, or None for print-wide steps.")
        .def("config_value", [](const SlicingPipelineContext& ctx, const std::string& key) -> py::object {
            if (ctx.print != nullptr)
                return config_value_or_none(ctx.print->full_print_config(), key);
            if (ctx.full_config != nullptr)
                return config_value_or_none(*ctx.full_config, key);
            return py::none();
        }, py::arg("key"),
           "Serialized value of a resolved (full) print config option for this slice, or None if absent.")
        .def("cancelled", &SlicingPipelineContext::cancelled);

    py::class_<SlicingPipelinePluginCapability, PluginCapabilityInterface,
               PySlicingPipelinePluginCapabilityTrampoline,
               std::shared_ptr<SlicingPipelinePluginCapability>>(slicing, "SlicingPipelineCapabilityBase")
        .def(py::init<>())
        .def("get_type", &SlicingPipelinePluginCapability::get_type)
        .def("execute", &SlicingPipelinePluginCapability::execute);
}

} // namespace Slic3r
