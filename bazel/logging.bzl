# SPDX-License-Identifier: Apache-2.0

"""Build rules for validated application logging deployment."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")

def _log_model_impl(ctx):
    model = ctx.actions.declare_file(ctx.label.name + ".json")
    header = ctx.actions.declare_file("generated/%s/ovf_logging.hpp" % ctx.label.name)
    arguments = ctx.actions.args()
    arguments.add("log-deployment")
    arguments.add("--cue", ctx.file._cue.path)
    arguments.add("--schema", ctx.file._schema.path)
    arguments.add("--deployment", ctx.file.deployment.path)
    arguments.add("--binding", ctx.file.binding.path)
    arguments.add("--output", model.path)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = [ctx.file._schema, ctx.file.deployment, ctx.file.binding],
        tools = [ctx.file._cue],
        outputs = [model],
        mnemonic = "OvfLogDeployment",
    )
    codegen = ctx.actions.args()
    codegen.add("log-cpp")
    codegen.add("--model", model.path)
    codegen.add("--output", header.path)
    ctx.actions.run(
        executable = ctx.executable._codegen,
        arguments = [codegen],
        inputs = [model],
        outputs = [header],
        mnemonic = "OvfLogCpp",
    )
    return [
        DefaultInfo(files = depset([model, header])),
        OutputGroupInfo(header = depset([header])),
    ]

_ovf_log_model = rule(
    implementation = _log_model_impl,
    attrs = {
        "deployment": attr.label(mandatory = True, allow_single_file = [".cue"]),
        "binding": attr.label(mandatory = True, allow_single_file = [".cue"]),
        "_builder": attr.label(default = Label("//tools:ovf_build"), executable = True, cfg = "exec"),
        "_codegen": attr.label(default = Label("//codegen:ovf_codegen"), executable = True, cfg = "exec"),
        "_cue": attr.label(default = Label("//bazel/host_tools:cue"), allow_single_file = True, cfg = "exec"),
        "_schema": attr.label(default = Label("//log/deployment/schema:deployment.cue"), allow_single_file = True),
    },
)

def ovf_log_application(name, deployment, binding = "//log/deployment/bindings:dlt.cue", visibility = None):
    """Generates an application's logging runtime from deployment intent.

    Args:
      name: Public generated C++ library target.
      deployment: Application-owned CUE deployment target.
      binding: System-selected logging binding policy target.
      visibility: Visibility of the generated library.
    """
    model = name + "_model"
    _ovf_log_model(name = model, deployment = deployment, binding = binding)
    native.filegroup(
        name = name + "_header",
        srcs = [":" + model],
        output_group = "header",
    )
    cc_library(
        name = name,
        hdrs = [":" + name + "_header"],
        include_prefix = "",
        strip_include_prefix = "generated/" + model,
        visibility = visibility,
        deps = ["//log:dlt_binding"],
    )
