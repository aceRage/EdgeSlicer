#include "PluginHooks.hpp"

#include "PluginManager.hpp"
#include "PluginResolver.hpp"
#include "PythonInterpreter.hpp"
#include "PythonPluginInterface.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"
#include "OrcaApiVersion.hpp"
#include "ipc/PluginHostClient.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/LifecycleEvents.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::plugin_hooks {
namespace {

void install_capability_resolver()
{
    ConfigBase::set_resolve_capability_fn([](const std::string& cap_name, const std::string& cap_type) {
        const auto ref = PluginManager::instance().resolve_capability_ref(cap_name, plugin_capability_type_from_string(cap_type));
        return ref ? create_full_ref(*ref) : std::string();
    });
}

void install_lifecycle_event_hook()
{
    set_lifecycle_hook_fn([](LifecycleEvent event, const LifecycleEventContext& ctx) {
        PluginManager::instance().dispatch_lifecycle_event(event, ctx);
    });
}

void run_post_process_plugins(const std::string& gcode_path,
                              const std::string& host,
                              const std::string& output_name,
                              const DynamicPrintConfig& config,
                              const std::function<bool()>& canceled)
{
    const auto* caps  = config.opt<ConfigOptionStrings>("slicing_pipeline_plugin");
    const auto* plugs = config.opt<ConfigOptionStrings>("plugins");
    if (caps == nullptr || caps->values.empty())
        return;

    const boost::filesystem::path gcode_file(gcode_path);

    auto execute_fn = [&](std::shared_ptr<SlicingPipelinePluginCapability> cap, const PluginCapabilityRef& ref) {
        SlicingPipelineContext ctx;
        ctx.orca_version = k_orca_api_version;
        ctx.step         = SlicingPipelineStepPlugin::psGCodePostProcess;
        ctx.gcode_path   = gcode_path;
        ctx.host         = host;
        ctx.output_name  = output_name;
        ctx.full_config  = &config;

        ExecutionResult exec_result;
        try {
            if (canceled && canceled())
                throw CanceledException();
            if (plugin_ipc::PluginHostClient::should_use_host()) {
                std::string host_error;
                exec_result = plugin_ipc::PluginHostClient::instance().execute_post_process(
                    cap->audit_plugin_key(), ref.capability_name, ctx, host_error);
                if (!host_error.empty() && exec_result.status == PluginResult::Success)
                    exec_result = ExecutionResult::failure(PluginResult::FatalError, host_error);
            } else {
                PythonGILState gil;
                if (!gil)
                    throw std::runtime_error("Python interpreter is shutting down");
                exec_result = cap->execute(ctx);
            }
        } catch (const CanceledException&) {
            throw;
        } catch (const std::exception& ex) {
            const std::string msg =
                Slic3r::format(_u8L("Post-processing plugin %1% raised an exception.\nError: %2%"), ref.capability_name, ex.what());
            BOOST_LOG_TRIVIAL(error) << msg;
            throw Slic3r::RuntimeError(msg);
        }

        if (exec_result.status == PluginResult::RecoverableError || exec_result.status == PluginResult::FatalError) {
            const std::string msg =
                Slic3r::format(_u8L("Post-processing plugin %1% failed.\nError: %2%"), ref.capability_name, exec_result.message);
            BOOST_LOG_TRIVIAL(error) << msg;
            throw Slic3r::RuntimeError(msg);
        }

        if (!exec_result.message.empty())
            BOOST_LOG_TRIVIAL(info) << "Post-processing plugin " << ref.capability_name << ": " << exec_result.message;

        if (!boost::filesystem::exists(gcode_file)) {
            const std::string msg = Slic3r::format(
                _u8L("Post-processing plugin %1% failed.\n\n"
                     "The post-processing plugin is expected to modify the G-code file %2% in place, but "
                     "the G-code file was deleted.\nPlease check the plugin implementation.\n"),
                ref.capability_name, gcode_path);
            BOOST_LOG_TRIVIAL(error) << msg;
            throw Slic3r::RuntimeError(msg);
        }
    };

    execute_capabilities_from_refs<SlicingPipelinePluginCapability>(*caps, plugs, PluginCapabilityType::SlicingPipeline, execute_fn);
}

void install_slicing_pipeline_hook()
{
    Print::set_slicing_pipeline_hook_fn(
        [](Print& print, const PrintObject* object, SlicingPipelineStepPlugin step) {
            const auto* caps  = print.config().option<ConfigOptionStrings>("slicing_pipeline_plugin");
            const auto* plugs = print.full_print_config().option<ConfigOptionStrings>("plugins");
            if (caps == nullptr || caps->values.empty())
                return;

            try {
                execute_capabilities_from_refs<SlicingPipelinePluginCapability>(
                *caps, plugs, PluginCapabilityType::SlicingPipeline,
                [&](std::shared_ptr<SlicingPipelinePluginCapability> cap, const PluginCapabilityRef& ref) {
                    ExecutionResult r;
                    try {
                        PythonGILState gil;
                        if (!gil)
                            throw std::runtime_error("Python interpreter is shutting down");
                        if (print.canceled())
                            throw CanceledException();
                        SlicingPipelineContext ctx;
                        ctx.orca_version = k_orca_api_version;
                        ctx.step         = step;
                        ctx.print        = &print;
                        ctx.object       = object;
                        r = cap->execute(ctx);
                    } catch (const CanceledException&) {
                        throw;
                    } catch (const std::exception& ex) {
                        throw SlicingError(Slic3r::format(_u8L("Slicing pipeline plugin '%1%' error: %2%"),
                                                          ref.capability_name, ex.what()));
                    }
                    if (r.status == PluginResult::RecoverableError || r.status == PluginResult::FatalError)
                        throw SlicingError(Slic3r::format(_u8L("Slicing pipeline plugin '%1%' error: %2%"),
                                                          ref.capability_name, r.message));
                    if (!r.message.empty()) {
                        static const char* const kStepNames[] = {
                            "posSlice", "posPerimeters", "posEstimateCurledExtrusions", "posPrepareInfill", "posInfill",
                            "posIroning", "posContouring", "posSupportMaterial", "posDetectOverhangsForLift",
                            "posSimplifyPath", "psWipeTower", "psSkirtBrim", "psGCodePostProcess"
                        };
                        const char* step_name = static_cast<size_t>(step) < sizeof(kStepNames) / sizeof(kStepNames[0])
                                                    ? kStepNames[static_cast<int>(step)] : "Unknown";
                        BOOST_LOG_TRIVIAL(info) << "Slicing pipeline plugin '" << ref.capability_name
                                                << "' [" << step_name << "]: " << r.message;
                    }
                });
            } catch (const CanceledException&) {
                throw;
            } catch (const SlicingError&) {
                throw;
            } catch (const std::exception& ex) {
                throw SlicingError(ex.what());
            }
        });
}

void install_gcode_post_process_hook()
{
    set_gcode_post_process_plugin_fn(&run_post_process_plugins);
}

} // namespace

void install()
{
    install_capability_resolver();
    install_slicing_pipeline_hook();
    install_gcode_post_process_hook();
    install_lifecycle_event_hook();
}

void uninstall()
{
    ConfigBase::set_resolve_capability_fn(nullptr);
    Print::set_slicing_pipeline_hook_fn(nullptr);
    set_gcode_post_process_plugin_fn(nullptr);
    set_lifecycle_hook_fn(nullptr);
}

} // namespace Slic3r::plugin_hooks
