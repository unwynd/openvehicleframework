# SPDX-License-Identifier: Apache-2.0

"""Validated persistence deployment and generated application facade."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")

OvfPerContractInfo = provider(
    doc = "Generated persistent-record contract artifacts.",
    fields = {"header": "Generated C++ header.", "ir": "Canonical persistent IR."},
)

def _per_contract_impl(ctx):
    ir = ctx.actions.declare_file(ctx.attr.output_name + ".ovf-per-ir.json")
    header = ctx.actions.declare_file("generated/%s/ovf_per_contract.hpp" % ctx.label.name)
    arguments = ctx.actions.args()
    arguments.add("per-contract")
    arguments.add("--smithy", ctx.file._smithy)
    arguments.add("--profile", ctx.file._base_profile)
    arguments.add("--profile", ctx.file._per_profile)
    for source in ctx.files.idl:
        arguments.add("--idl", source)
    arguments.add("--ir", ir)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset(ctx.files.idl + [ctx.file._base_profile, ctx.file._per_profile, ctx.file._smithy]),
        tools = ctx.files._smithy_runtime,
        outputs = [ir],
        mnemonic = "OvfPerContractCompile",
    )
    codegen = ctx.actions.args()
    codegen.add("per-contract-cpp")
    codegen.add("--model", ir)
    codegen.add("--output", header)
    ctx.actions.run(
        executable = ctx.executable._codegen,
        arguments = [codegen],
        inputs = [ir],
        outputs = [header],
        mnemonic = "OvfPerContractCpp",
    )
    return [
        DefaultInfo(files = depset([ir, header])),
        OvfPerContractInfo(header = header, ir = ir),
        OutputGroupInfo(header = depset([header]), ir = depset([ir])),
    ]

_ovf_per_contract = rule(
    implementation = _per_contract_impl,
    attrs = {
        "idl": attr.label_list(mandatory = True, allow_files = [".smithy"]),
        "output_name": attr.string(mandatory = True),
        "_base_profile": attr.label(default = Label("//com/model:ovf-profile.smithy"), allow_single_file = True),
        "_per_profile": attr.label(default = Label("//per/model:ovf-per-profile.smithy"), allow_single_file = True),
        "_builder": attr.label(default = Label("//tools:ovf_build"), executable = True, cfg = "exec"),
        "_codegen": attr.label(default = Label("//codegen:ovf_codegen"), executable = True, cfg = "exec"),
        "_smithy": attr.label(default = Label("//bazel/host_tools:smithy"), allow_single_file = True, cfg = "exec"),
        "_smithy_runtime": attr.label(default = Label("//bazel/host_tools:smithy_runtime"), cfg = "exec"),
    },
)

def ovf_per_contract(name, idl, output_name, visibility = None):
    """Compiles typed persistent records and exposes generated C++ codecs.

    Args:
      name: Public generated C++ library target.
      idl: Smithy persistent-record sources.
      output_name: Stable generated artifact base name.
      visibility: Visibility of the generated library.
    """
    model = name + "_model"
    _ovf_per_contract(name = model, idl = idl, output_name = output_name)
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
        deps = ["//per:codec"],
    )

def _per_model_impl(ctx):
    model = ctx.actions.declare_file(ctx.label.name + ".json")
    header = ctx.actions.declare_file("generated/%s/ovf_persistence.hpp" % ctx.label.name)
    arguments = ctx.actions.args()
    arguments.add("per-deployment")
    arguments.add("--cue", ctx.file._cue)
    arguments.add("--schema", ctx.file._schema)
    arguments.add("--deployment", ctx.file.deployment)
    arguments.add("--binding", ctx.file.binding)
    arguments.add("--namespace", ctx.attr.cpp_namespace)
    arguments.add("--output", model)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = [ctx.file._schema, ctx.file.deployment, ctx.file.binding],
        tools = [ctx.file._cue],
        outputs = [model],
        mnemonic = "OvfPerDeployment",
    )
    codegen = ctx.actions.args()
    codegen.add("per-cpp")
    codegen.add("--model", model)
    codegen.add("--output", header)
    ctx.actions.run(
        executable = ctx.executable._codegen,
        arguments = [codegen],
        inputs = [model],
        outputs = [header],
        mnemonic = "OvfPerCpp",
    )
    return [
        DefaultInfo(files = depset([model, header])),
        OutputGroupInfo(header = depset([header]), model = depset([model])),
    ]

_ovf_per_model = rule(
    implementation = _per_model_impl,
    attrs = {
        "deployment": attr.label(mandatory = True, allow_single_file = [".cue"]),
        "binding": attr.label(mandatory = True, allow_single_file = [".cue"]),
        "cpp_namespace": attr.string(mandatory = True),
        "_builder": attr.label(default = Label("//tools:ovf_build"), executable = True, cfg = "exec"),
        "_codegen": attr.label(default = Label("//codegen:ovf_codegen"), executable = True, cfg = "exec"),
        "_cue": attr.label(default = Label("//bazel/host_tools:cue"), allow_single_file = True, cfg = "exec"),
        "_schema": attr.label(default = Label("//per/deployment/schema:deployment.cue"), allow_single_file = True),
    },
)

def ovf_per_application(name, deployment, cpp_namespace, binding = "//per/deployment/bindings:sqlite.cue", visibility = None):
    """Generates a persistence facade from application intent and platform binding.

    Args:
      name: Public generated C++ library target.
      deployment: Application-owned CUE deployment target.
      cpp_namespace: Namespace for generated named-store functions.
      binding: Platform-selected persistence provider policy.
      visibility: Visibility of the generated library.
    """
    model = name + "_model"
    _ovf_per_model(
        name = model,
        deployment = deployment,
        binding = binding,
        cpp_namespace = cpp_namespace,
    )
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
        deps = ["//per:runtime"],
    )
