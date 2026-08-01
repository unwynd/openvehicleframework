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
        "source": "Authored CUE deployment input.",
        "ir": "Resolved canonical deployment JSON.",
        "plan": "Canonical target runtime plan.",
        "report": "Validation report.",
        "header": "Generated transport-neutral deployment facade.",
        "contract": "OvfContractInfo used by this deployment.",
    },
)

OvfApplicationInfo = provider(
    doc = "Application-owned deployment and its packaged executable identity.",
    fields = {
        "deployment": "Authored application deployment CUE.",
        "executable": "Built application executable file.",
        "executable_target": "Label of the application executable target.",
        "install_path": "Executable path relative to the deployed filesystem root.",
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
    arguments.add("--cue", ctx.file._cue)
    arguments.add("--schema", ctx.file._deployment_schema)
    arguments.add("--contract", contract.ir)
    arguments.add("--deployment", ctx.file.deployment)
    arguments.add("--platform", ctx.file.platform)
    arguments.add("--deployment-ir", ctx.outputs.deployment_ir)
    for profile in ctx.files.profiles:
        arguments.add("--profile", profile)
    arguments.add("--plan", ctx.outputs.plan)
    arguments.add("--report", ctx.outputs.report)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset(
            [
                contract.ir,
                ctx.file.deployment,
                ctx.file.platform,
                ctx.file._deployment_schema,
            ] + ctx.files.profiles,
        ),
        tools = [ctx.file._cue],
        outputs = [ctx.outputs.deployment_ir, ctx.outputs.plan, ctx.outputs.report],
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
        ir = ctx.outputs.deployment_ir,
        plan = ctx.outputs.plan,
        report = ctx.outputs.report,
        header = ctx.outputs.header,
        contract = contract,
    )
    return [
        DefaultInfo(files = depset([
            ctx.outputs.deployment_ir,
            ctx.outputs.plan,
            ctx.outputs.report,
            ctx.outputs.header,
        ])),
        info,
        OutputGroupInfo(
            header = depset([ctx.outputs.header]),
            ir = depset([ctx.outputs.deployment_ir]),
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
            allow_single_file = [".cue"],
        ),
        "platform": attr.label(
            mandatory = True,
            allow_single_file = [".cue"],
            doc = "Platform provider policy composed with deployment intent.",
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
        "_cue": attr.label(
            default = Label("//bazel/host_tools:cue"),
            allow_single_file = True,
            cfg = "exec",
        ),
        "_deployment_schema": attr.label(
            default = Label("//com/deployment/schema:deployment.cue"),
            allow_single_file = True,
        ),
        "_codegen": attr.label(
            default = Label("//codegen:ovf_codegen"),
            executable = True,
            cfg = "exec",
        ),
    },
    outputs = {
        "deployment_ir": "%{name}.ovf-deployment.json",
        "plan": "%{name}.plan.json",
        "report": "%{name}.validation.json",
        "header": "generated/%{name}/ovf_deployment.hpp",
    },
    doc = "Validates deployment and emits the canonical target runtime plan.",
)

def _application_package_impl(ctx):
    deployments = [target[OvfDeploymentInfo] for target in ctx.attr.deployments]
    output = ctx.outputs.bundle
    arguments = ctx.actions.args()
    arguments.add("package")
    arguments.add("--name", ctx.attr.application_name)
    arguments.add("--executable", ctx.executable.application)
    arguments.add("--install-path", ctx.attr.install_path)
    for deployment in deployments:
        arguments.add("--contract", deployment.contract.ir)
        arguments.add("--deployment", deployment.ir)
        arguments.add("--plan", deployment.plan)
        arguments.add("--report", deployment.report)
    arguments.add("--output", output)
    inputs = [ctx.executable.application]
    for deployment in deployments:
        inputs.extend([
            deployment.contract.ir,
            deployment.ir,
            deployment.plan,
            deployment.report,
        ])
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset(inputs),
        outputs = [output],
        mnemonic = "OvfApplicationPackage",
        progress_message = "Packaging OVF application %{label}",
    )
    return [DefaultInfo(files = depset([output]))]

ovf_application_package = rule(
    implementation = _application_package_impl,
    attrs = {
        "application_name": attr.string(mandatory = True),
        "install_path": attr.string(mandatory = True),
        "application": attr.label(mandatory = True, executable = True, cfg = "target"),
        "deployments": attr.label_list(mandatory = True, providers = [OvfDeploymentInfo]),
        "_builder": attr.label(
            default = Label("//tools:ovf_build"),
            executable = True,
            cfg = "exec",
        ),
    },
    outputs = {"bundle": "%{name}.tar"},
    doc = "Creates a deterministic target bundle which excludes framework middleware.",
)

def _application_info_impl(ctx):
    return [
        DefaultInfo(files = depset([ctx.file.deployment, ctx.executable.application])),
        OvfApplicationInfo(
            deployment = ctx.file.deployment,
            executable = ctx.executable.application,
            executable_target = ctx.attr.executable_target,
            install_path = ctx.attr.install_path,
        ),
    ]

ovf_application_info = rule(
    implementation = _application_info_impl,
    attrs = {
        "deployment": attr.label(
            mandatory = True,
            allow_single_file = [".cue"],
        ),
        "application": attr.label(
            mandatory = True,
            executable = True,
            cfg = "target",
        ),
        "executable_target": attr.string(
            mandatory = True,
        ),
        "install_path": attr.string(
            mandatory = True,
        ),
    },
    doc = "Exports an application's deployment and executable to system integration.",
)

def _application_facade_impl(ctx):
    header = ctx.outputs.header
    includes = "\n".join([
        "#include \"%s/ovf_deployment.hpp\"" % interface
        for interface in ctx.attr.interfaces
    ])
    transports = ",\n        ".join([
        "%s_deployment::Transport()" % interface
        for interface in ctx.attr.interfaces
    ])
    routes = "\n\n".join([
        """inline auto %s() -> ovf::com::RouteBinding {
  return %s_deployment::Route();
}""" % (interface, interface)
        for interface in ctx.attr.interfaces
    ])
    ctx.actions.write(
        header,
        """#pragma once

// SPDX-License-Identifier: Apache-2.0
// Generated by the OVF Bazel application API; do not edit.

#include <string>

%s

namespace %s {

inline auto CreateRuntime(std::string instance_name) -> ovf::com::ApplicationRuntime {
  return ovf::com::ApplicationRuntime(
      {.instance_name = std::move(instance_name), .logger = {}, .dispatcher = {}},
      {
        %s
      });
}

%s

} // namespace %s
""" % (includes, "ovf::app", transports, routes, "ovf::app"),
    )
    return [
        DefaultInfo(files = depset([header])),
        OutputGroupInfo(header = depset([header])),
    ]

ovf_application_facade = rule(
    implementation = _application_facade_impl,
    attrs = {
        "interfaces": attr.string_list(mandatory = True),
    },
    outputs = {"header": "generated/%{name}/ovf_application.hpp"},
    doc = "Generates the typed runtime and instance facade for one application.",
)

def _ovf_cc_application_impl(
        name,
        srcs,
        interfaces,
        application_name = None,
        execution_install_path = None,
        hdrs = [],
        deps = [],
        copts = [],
        defines = [],
        data = [],
        tags = [],
        visibility = None,
        **kwargs):
    """Expands interface targets into generated application dependencies.

    Each interface dictionary requires `name`, `idl`, `deployment`, and `platform`.
    It may also provide `service`, `deployment_namespace`, and `profiles`.
    Generated headers are included as `<name>/ovf_contract.hpp` and
    `<name>/ovf_deployment.hpp`.

    Args:
      name: Application target name.
      application_name: Stable deployment name, independent of the Bazel target variant.
      srcs: C++ source files owned by the application.
      interfaces: Interface role dictionaries composed into the process.
      hdrs: Application-owned headers.
      deps: Additional C++ dependencies.
      copts: Additional compiler options.
      defines: Additional preprocessor definitions.
      data: Runtime data files.
      tags: Bazel tags propagated to generated targets.
      execution_install_path: Executable path relative to the deployed filesystem root.
      visibility: Visibility of public targets.
      **kwargs: Additional attributes forwarded to the application binary.
    """
    interface_libraries = []
    deployment_rules = []
    deployment_libraries = []
    artifacts = []
    for interface in interfaces:
        interface_name = interface["name"]
        prefix = name + "_" + interface_name
        contract_rule = prefix + "_contract_codegen"
        contract_header = prefix + "_contract_header"
        contract_library = prefix + "_contract"
        deployment_rule = prefix + "_deployment"
        deployment_header = prefix + "_deployment_header"

        ovf_contract(
            name = contract_rule,
            idl = interface["idl"],
            service = interface.get("service", ""),
            output_name = prefix,
            visibility = ["//visibility:private"],
        )
        native.filegroup(
            name = contract_header,
            srcs = [":" + contract_rule],
            output_group = "header",
            visibility = ["//visibility:private"],
        )
        cc_library(
            name = contract_library,
            hdrs = [":" + contract_header],
            include_prefix = interface_name,
            strip_include_prefix = "generated/" + contract_rule,
            copts = ["-std=c++20"],
            deps = [Label("//com:api")],
            visibility = ["//visibility:private"],
        )

        deployment_arguments = {
            "name": deployment_rule,
            "contract": ":" + contract_rule,
            "cpp_namespace": interface.get(
                "deployment_namespace",
                prefix + "_deployment",
            ),
            "deployment": interface["deployment"],
            "platform": interface["platform"],
            "visibility": ["//visibility:private"],
        }
        if interface.get("profiles") != None:
            deployment_arguments["profiles"] = interface["profiles"]
        ovf_deployment(**deployment_arguments)
        native.filegroup(
            name = deployment_header,
            srcs = [":" + deployment_rule],
            output_group = "header",
            visibility = ["//visibility:private"],
        )
        cc_library(
            name = prefix + "_deployment_api",
            hdrs = [":" + deployment_header],
            include_prefix = interface_name,
            strip_include_prefix = "generated/" + deployment_rule,
            deps = [Label("//com:api")],
            visibility = ["//visibility:private"],
        )
        interface_libraries.extend([
            ":" + contract_library,
            ":" + prefix + "_deployment_api",
        ])
        deployment_libraries.append(":" + prefix + "_deployment_api")
        deployment_rules.append(":" + deployment_rule)
        artifacts.extend([
            ":" + contract_rule,
            ":" + deployment_rule,
            interface["deployment"],
            interface["platform"],
        ] + interface["idl"])

    facade_rule = name + "_application_facade"
    ovf_application_facade(
        name = facade_rule,
        interfaces = [interface["name"] for interface in interfaces],
        visibility = ["//visibility:private"],
    )
    artifacts.append(":" + facade_rule)
    cc_library(
        name = name + "_application_api",
        hdrs = [":" + facade_rule],
        includes = ["generated/" + facade_rule],
        deps = deployment_libraries + [Label("//com:api")],
        visibility = ["//visibility:private"],
    )
    cc_binary(
        name = name,
        srcs = srcs + hdrs,
        deps = interface_libraries + [":" + name + "_application_api"] + deps,
        copts = ["-std=c++20"] + copts,
        defines = defines,
        data = data,
        tags = tags,
        visibility = visibility,
        linkstatic = False,
        **kwargs
    )
    unique_artifacts = []
    for artifact in artifacts + hdrs:
        if artifact not in unique_artifacts:
            unique_artifacts.append(artifact)
    native.filegroup(
        name = name + "_artifacts",
        srcs = unique_artifacts,
        visibility = visibility,
    )
    ovf_application_package(
        name = name + "_bundle",
        application_name = application_name or name,
        application = ":" + name,
        deployments = deployment_rules,
        install_path = execution_install_path or "opt/%s/bin/%s" % (
            application_name or name,
            application_name or name,
        ),
        visibility = visibility,
    )

def ovf_cc_application(
        name,
        srcs,
        interfaces,
        deployment,
        platform,
        application_name = None,
        hdrs = [],
        deps = [],
        copts = [],
        defines = [],
        data = [],
        tags = [],
        execution_install_path = None,
        visibility = None,
        **kwargs):
    """Builds a C++ application using one or more interface targets.

    Contract targets encapsulate Smithy sources. The application owns one CUE
    deployment containing all of its provider and consumer roles. Platform
    policy resolves each role's logical transport selection.

    Args:
      name: Application target name.
      application_name: Stable deployment name, independent of the Bazel target variant.
      srcs: C++ source files owned by the application.
      interfaces: Public OVF interface targets consumed by the application.
      deployment: The application's single CUE deployment model.
      platform: Platform policy target used to resolve logical transports.
      hdrs: Application-owned headers.
      deps: Additional C++ dependencies.
      copts: Additional compiler options.
      defines: Additional preprocessor definitions.
      data: Runtime data files.
      tags: Bazel tags propagated to generated targets.
      execution_install_path: Executable path relative to the deployed filesystem root.
      visibility: Visibility of public targets.
      **kwargs: Additional attributes forwarded to the application binary.
    """
    specifications = []
    names = []
    for target in interfaces:
        interface_name = str(target).split(":")[-1].split("/")[-1]
        if interface_name in names:
            fail("duplicate interface target name: " + interface_name)
        names.append(interface_name)
        specifications.append({
            "name": interface_name,
            "deployment": deployment,
            "deployment_namespace": interface_name + "_deployment",
            "idl": [target],
            "platform": platform,
        })
    _ovf_cc_application_impl(
        name = name,
        srcs = srcs,
        interfaces = specifications,
        application_name = application_name,
        execution_install_path = execution_install_path,
        hdrs = hdrs,
        deps = deps,
        copts = copts,
        defines = defines,
        data = data,
        tags = tags,
        visibility = visibility,
        **kwargs
    )
    ovf_application_info(
        name = name + "_execution_deployment",
        deployment = deployment,
        application = ":" + name,
        executable_target = ":" + name,
        install_path = execution_install_path or "opt/%s/bin/%s" % (
            application_name or name,
            application_name or name,
        ),
        visibility = visibility,
    )
