# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Pipeline Stamp"
# description = "Appends a fixed comment block at psGCodePostProcess."
# author = "tests"
# version = "1.0.0"
# type = "slicing-pipeline"
# ///
import orca

STAMP = (
    "; EDGE_PIPELINE_STAMP begin\n"
    "; EdgeSlicer pipeline plugin stamp v1\n"
    "; EDGE_PIPELINE_STAMP end\n"
)


class Stamp(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "Stamp"

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.psGCodePostProcess:
            return orca.ExecutionResult.success()
        if not ctx.gcode_path:
            return orca.ExecutionResult.failure(orca.PluginResult.FatalError, "missing gcode_path")
        with open(ctx.gcode_path, "a", encoding="utf-8") as f:
            f.write(STAMP)
        return orca.ExecutionResult.success("stamped")


@orca.plugin
class PipelineStampPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Stamp)
