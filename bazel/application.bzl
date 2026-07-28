# SPDX-License-Identifier: Apache-2.0

"""Public Bazel API for building transport-neutral OVF C++ applications."""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

OvfContractInfo = provider(
    doc = "Generated, validated communication contract artifacts.",
    fields = {
        "header": "Generated C++ header.",
        "ir": "Canonical communication IR.",
        "metadata": "Contract build metadata.",
    },
)

OvfDeploymentInfo = provider(
    doc = "Validated target deployment artifacts.",
    fields = {
        "source": "Authored deployment input.",
        "plan": "Canonical target runtime plan.",
        "report": "Validation report.",
        "header": "Generated transport-neutral deployment facade.",
        "contract": "OvfContractInfo used by this deployment.",
    },
)

def _contract_impl(ctx):
    ir = ctx.actions.declare_file(ctx.attr.output_name + ".ovf-ir.json")
    header = ctx.actions.declare_file(
        "generated/" + ctx.label.name + "/ovf_contract.hpp",
    )
    metadata = ctx.actions.declare_file(ctx.attr.output_name + ".contract.json")
    arguments = ctx.actions.args()
    arguments.add("contract")
    arguments.add("--smithy", ctx.file._smithy)
    arguments.add("--profile", ctx.file._ovf_profile)
    for source in ctx.files.idl:
        arguments.add("--idl", source)
    if ctx.attr.service:
        arguments.add("--service", ctx.attr.service)
    arguments.add("--ir", ir)
    arguments.add("--generated-header", header.basename)
    arguments.add("--metadata", metadata)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset(ctx.files.idl + [ctx.file._ovf_profile, ctx.file._smithy]),
        tools = ctx.files._smithy_runtime,
        outputs = [ir, metadata],
        mnemonic = "OvfContractCompile",
        progress_message = "Compiling OVF contract %{label}",
    )
    codegen_arguments = ctx.actions.args()
    codegen_arguments.add(ir)
    codegen_arguments.add("--output")
    codegen_arguments.add(header)
    ctx.actions.run(
        executable = ctx.executable._codegen,
        arguments = [codegen_arguments],
        inputs = [ir],
        outputs = [header],
        mnemonic = "OvfCppCodegen",
        progress_message = "Generating C++ contract %{label}",
    )
    return [
        DefaultInfo(files = depset([ir, header, metadata])),
        OvfContractInfo(header = header, ir = ir, metadata = metadata),
        OutputGroupInfo(header = depset([header]), ir = depset([ir]), metadata = depset([metadata])),
    ]

ovf_contract = rule(
    implementation = _contract_impl,
    attrs = {
        "idl": attr.label_list(
            allow_files = [".smithy"],
            mandatory = True,
            doc = "Smithy contract source files.",
        ),
        "service": attr.string(
            doc = "Fully-qualified service shape when the IDL contains more than one service.",
        ),
        "output_name": attr.string(
            mandatory = True,
            doc = "Stable base name for generated files.",
        ),
        "_builder": attr.label(
            default = Label("//tools:ovf_build"),
            executable = True,
            cfg = "exec",
        ),
        "_codegen": attr.label(
            default = Label("//codegen:ovf_codegen"),
            executable = True,
            cfg = "exec",
        ),
        "_ovf_profile": attr.label(
            default = Label("//com/model:ovf-profile.smithy"),
            allow_single_file = True,
        ),
        "_smithy": attr.label(
            default = Label("//bazel/host_tools:smithy"),
            allow_single_file = True,
            cfg = "exec",
        ),
        "_smithy_runtime": attr.label(
            default = Label("//bazel/host_tools:smithy_runtime"),
            cfg = "exec",
        ),
    },
    doc = "Compiles Smithy IDL into canonical IR and deterministic C++.",
)

def _deployment_impl(ctx):
    contract = ctx.attr.contract[OvfContractInfo]
    arguments = ctx.actions.args()
    arguments.add("deployment")
    arguments.add("--contract", contract.ir)
    arguments.add("--deployment", ctx.file.deployment)
    for profile in ctx.files.profiles:
        arguments.add("--profile", profile)
    arguments.add("--plan", ctx.outputs.plan)
    arguments.add("--report", ctx.outputs.report)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset(
            [contract.ir, ctx.file.deployment] + ctx.files.profiles,
        ),
        outputs = [ctx.outputs.plan, ctx.outputs.report],
        mnemonic = "OvfDeploymentCompile",
        progress_message = "Validating OVF deployment %{label}",
    )
    header_arguments = ctx.actions.args()
    header_arguments.add("deployment-cpp")
    header_arguments.add("--plan")
    header_arguments.add(ctx.outputs.plan)
    header_arguments.add("--namespace")
    header_arguments.add(ctx.attr.cpp_namespace)
    header_arguments.add("--output")
    header_arguments.add(ctx.outputs.header)
    ctx.actions.run(
        executable = ctx.executable._codegen,
        arguments = [header_arguments],
        inputs = [ctx.outputs.plan],
        outputs = [ctx.outputs.header],
        mnemonic = "OvfDeploymentCpp",
        progress_message = "Generating deployment facade %{label}",
    )
    info = OvfDeploymentInfo(
        source = ctx.file.deployment,
        plan = ctx.outputs.plan,
        report = ctx.outputs.report,
        header = ctx.outputs.header,
        contract = contract,
    )
    return [
        DefaultInfo(files = depset([ctx.outputs.plan, ctx.outputs.report, ctx.outputs.header])),
        info,
        OutputGroupInfo(
            header = depset([ctx.outputs.header]),
            plan = depset([ctx.outputs.plan]),
            report = depset([ctx.outputs.report]),
        ),
    ]

ovf_deployment = rule(
    implementation = _deployment_impl,
    attrs = {
        "contract": attr.label(
            mandatory = True,
            providers = [OvfContractInfo],
        ),
        "cpp_namespace": attr.string(mandatory = True),
        "deployment": attr.label(
            mandatory = True,
            allow_single_file = [".json"],
        ),
        "profiles": attr.label_list(
            allow_files = [".json"],
            default = [
                Label("//com/deployment/profiles:inproc.json"),
                Label("//com/deployment/profiles:iceoryx2.json"),
                Label("//com/deployment/profiles:vsomeip.json"),
                Label("//com/deployment/profiles:cyclonedds.json"),
            ],
        ),
        "_builder": attr.label(
            default = Label("//tools:ovf_build"),
            executable = True,
            cfg = "exec",
        ),
        "_codegen": attr.label(
            default = Label("//codegen:ovf_codegen"),
            executable = True,
            cfg = "exec",
        ),
    },
    outputs = {
        "plan": "%{name}.plan.json",
        "report": "%{name}.validation.json",
        "header": "generated/%{name}/ovf_deployment.hpp",
    },
    doc = "Validates deployment and emits the canonical target runtime plan.",
)

def _application_package_impl(ctx):
    deployment = ctx.attr.deployment[OvfDeploymentInfo]
    output = ctx.outputs.bundle
    arguments = ctx.actions.args()
    arguments.add("package")
    arguments.add("--name", ctx.attr.application_name)
    arguments.add("--executable", ctx.executable.application)
    arguments.add("--contract", deployment.contract.ir)
    arguments.add("--deployment", deployment.source)
    arguments.add("--plan", deployment.plan)
    arguments.add("--report", deployment.report)
    arguments.add("--output", output)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset([
            ctx.executable.application,
            deployment.contract.ir,
            deployment.source,
            deployment.plan,
            deployment.report,
        ]),
        outputs = [output],
        mnemonic = "OvfApplicationPackage",
        progress_message = "Packaging OVF application %{label}",
    )
    return [DefaultInfo(files = depset([output]))]

ovf_application_package = rule(
    implementation = _application_package_impl,
    attrs = {
        "application_name": attr.string(mandatory = True),
        "application": attr.label(mandatory = True, executable = True, cfg = "target"),
        "deployment": attr.label(mandatory = True, providers = [OvfDeploymentInfo]),
        "_builder": attr.label(
            default = Label("//tools:ovf_build"),
            executable = True,
            cfg = "exec",
        ),
    },
    outputs = {"bundle": "%{name}.tar"},
    doc = "Creates a deterministic target bundle which excludes framework middleware.",
)

def ovf_cc_application(
        name,
        srcs,
        idl,
        deployment,
        deployment_namespace = "",
        hdrs = [],
        service = "",
        deps = [],
        profiles = None,
        copts = [],
        defines = [],
        data = [],
        tags = [],
        visibility = None,
        **kwargs):
    """Builds a clean C++ application plus all generated and target artifacts.

    Public targets:
      `<name>`: application executable;
      `<name>_contract`: generated C++ contract library;
      `<name>_artifacts`: IR, generated header/metadata, plan and validation;
      `<name>_bundle`: target application tar, without OVF middleware binaries.

    Args:
      name: Name of the application target.
      srcs: C++ sources owned by the application.
      idl: Smithy files defining the service contract.
      deployment: Deployment model selecting identifiers and profiles.
      deployment_namespace: Optional stable C++ namespace for deployment functions.
      hdrs: Application-owned headers.
      service: Fully qualified service shape to generate.
      deps: Additional C++ dependencies.
      profiles: Deployment profiles used to generate target configuration.
      copts: Additional C++ compiler options.
      defines: Additional preprocessor definitions.
      data: Runtime data files packaged with the application.
      tags: Bazel tags propagated to generated targets.
      visibility: Bazel visibility of public generated targets.
      **kwargs: Additional attributes forwarded to the application binary.
    """
    contract_rule = name + "_contract_codegen"
    contract_header = name + "_contract_header"
    contract_ir = name + "_contract_ir"
    contract_metadata = name + "_contract_metadata"
    contract_library = name + "_contract"
    deployment_rule = name + "_deployment"

    ovf_contract(
        name = contract_rule,
        idl = idl,
        service = service,
        output_name = name,
        visibility = ["//visibility:private"],
    )
    native.filegroup(
        name = contract_header,
        srcs = [":" + contract_rule],
        output_group = "header",
        visibility = ["//visibility:private"],
    )
    native.filegroup(
        name = contract_ir,
        srcs = [":" + contract_rule],
        output_group = "ir",
    )
    native.filegroup(
        name = contract_metadata,
        srcs = [":" + contract_rule],
        output_group = "metadata",
    )
    cc_library(
        name = contract_library,
        hdrs = [":" + contract_header],
        includes = ["generated/" + contract_rule],
        copts = ["-std=c++20"],
        deps = [Label("//com:api")],
        visibility = visibility,
    )
    deployment_arguments = {
        "name": deployment_rule,
        "contract": ":" + contract_rule,
        "cpp_namespace": deployment_namespace or (name + "_deployment"),
        "deployment": deployment,
        "visibility": ["//visibility:private"],
    }
    if profiles != None:
        deployment_arguments["profiles"] = profiles
    ovf_deployment(**deployment_arguments)
    cc_library(
        name = name + "_deployment_api",
        hdrs = [":" + deployment_rule],
        includes = ["generated/" + deployment_rule],
        deps = [Label("//com:api")],
        visibility = ["//visibility:private"],
    )
    cc_binary(
        name = name,
        srcs = srcs + hdrs,
        deps = [":" + contract_library, ":" + name + "_deployment_api"] + deps,
        copts = ["-std=c++20"] + copts,
        defines = defines,
        data = data,
        tags = tags,
        visibility = visibility,
        linkstatic = False,
        **kwargs
    )
    native.filegroup(
        name = name + "_artifacts",
        srcs = [
            ":" + contract_rule,
            ":" + deployment_rule,
            deployment,
        ] + idl + hdrs,
        visibility = visibility,
    )
    ovf_application_package(
        name = name + "_bundle",
        application_name = name,
        application = ":" + name,
        deployment = ":" + deployment_rule,
        visibility = visibility,
    )
