# SPDX-License-Identifier: Apache-2.0

"""Public Bazel API for building transport-neutral OVF C++ applications."""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//bazel:logging.bzl", "ovf_internal_log_facade")
load("//bazel:persistency.bzl", "OvfPerContractInfo", "ovf_internal_persistence_facade", _ovf_persistent_schema = "ovf_persistent_schema")

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
        "name": "Stable application deployment name.",
        "model": "Portable generated application communication model.",
        "contracts": "Named contract providers consumed by this application.",
    },
)

OvfApplicationModelInfo = provider(
    doc = "Portable application communication model and generated C++ facade.",
    fields = {"model": "Canonical application model JSON.", "header": "Generated facade header."},
)

OvfCommunicationDeploymentInfo = provider(
    doc = "System-resolved per-application communication runtime deployment directory.",
    fields = {"directory": "Directory containing one runtime JSON file per application."},
)

def _communication_deployment_impl(ctx):
    output = ctx.actions.declare_directory(ctx.label.name + ".runtime")
    arguments = ctx.actions.args()
    arguments.add("communication-deployment")
    arguments.add("--cue", ctx.file._cue)
    inputs = []
    for binding in ctx.files.bindings:
        arguments.add("--binding", binding)
        inputs.append(binding)
    for application in ctx.attr.applications:
        info = application[OvfApplicationInfo]
        arguments.add("--application-name", info.name)
        arguments.add("--application-model", info.model)
        arguments.add("--application-deployment", info.deployment)
        inputs.extend([info.model, info.deployment])
        for contract in info.contracts:
            arguments.add("--contract", info.name + "=" + contract.info.ir.path)
            inputs.append(contract.info.ir)
    for profile in ctx.files.profiles:
        arguments.add("--profile", profile)
    arguments.add("--output", output.path)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset(inputs + ctx.files.profiles),
        tools = [ctx.file._cue],
        outputs = [output],
        mnemonic = "OvfCommunicationDeployment",
        progress_message = "Resolving system communication deployment %{label}",
    )
    return [DefaultInfo(files = depset([output])), OvfCommunicationDeploymentInfo(directory = output)]

ovf_communication_deployment = rule(
    implementation = _communication_deployment_impl,
    attrs = {
        "applications": attr.label_list(mandatory = True, providers = [OvfApplicationInfo]),
        "bindings": attr.label_list(mandatory = True, allow_files = [".cue"]),
        "profiles": attr.label_list(allow_files = [".json"], default = [
            Label("//com/deployment/profiles:inproc.json"),
            Label("//com/deployment/profiles:iceoryx2.json"),
            Label("//com/deployment/profiles:vsomeip.json"),
            Label("//com/deployment/profiles:cyclonedds.json"),
        ]),
        "_builder": attr.label(default = Label("//tools:ovf_build"), executable = True, cfg = "exec"),
        "_cue": attr.label(default = Label("//bazel/host_tools:cue"), allow_single_file = True, cfg = "exec"),
    },
    doc = "Resolves application intent against system-owned communication bindings.",
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

def _interface_export_impl(ctx):
    contract = ctx.attr.contract[OvfContractInfo]
    library = ctx.attr.library[CcInfo]
    return [
        DefaultInfo(files = ctx.attr.contract[DefaultInfo].files),
        CcInfo(
            compilation_context = library.compilation_context,
            linking_context = library.linking_context,
        ),
        OvfContractInfo(header = contract.header, ir = contract.ir, metadata = contract.metadata),
        OutputGroupInfo(
            header = depset([contract.header]),
            ir = depset([contract.ir]),
            metadata = depset([contract.metadata]),
        ),
    ]

_interface_export = rule(
    implementation = _interface_export_impl,
    attrs = {
        "contract": attr.label(mandatory = True, providers = [OvfContractInfo]),
        "library": attr.label(mandatory = True, providers = [CcInfo]),
    },
)

def ovf_interface(name, srcs, service = "", visibility = None):
    """Defines one reusable Smithy communication interface.

    Args:
      name: Stable interface target and generated include prefix.
      srcs: Smithy interface source files.
      service: Optional fully-qualified service shape selector.
      visibility: Visibility of the exported interface target.
    """
    contract = name + "_contract"
    header = name + "_header"
    library = name + "_cc"
    ovf_contract(
        name = contract,
        idl = srcs,
        service = service,
        output_name = name.replace("_", "-"),
        visibility = ["//visibility:private"],
    )
    native.filegroup(
        name = header,
        srcs = [":" + contract],
        output_group = "header",
        visibility = ["//visibility:private"],
    )
    cc_library(
        name = library,
        hdrs = [":" + header],
        include_prefix = name,
        strip_include_prefix = "generated/" + contract,
        deps = ["//com:api"],
        visibility = ["//visibility:private"],
    )
    _interface_export(
        name = name,
        contract = ":" + contract,
        library = ":" + library,
        visibility = visibility,
    )

def ovf_persistent_schema(name, srcs, visibility = None):
    """Defines one reusable Smithy persistent schema.

    Args:
      name: Stable schema target and generated include prefix.
      srcs: Smithy persistent-record source files.
      visibility: Visibility of the exported schema target.
    """
    _ovf_persistent_schema(name = name, srcs = srcs, visibility = visibility)

def _deployment_impl(ctx):
    contract = ctx.attr.contract[OvfContractInfo]
    arguments = ctx.actions.args()
    arguments.add("deployment")
    arguments.add("--cue", ctx.file._cue)
    arguments.add("--schema", ctx.file._deployment_schema)
    arguments.add("--contract", contract.ir)
    arguments.add("--deployment", ctx.file.deployment)
    arguments.add("--binding", ctx.file.binding)
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
                ctx.file.binding,
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
        "binding": attr.label(
            mandatory = True,
            allow_single_file = [".cue"],
            doc = "Communication binding policy composed with deployment intent.",
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
    contracts = [target[OvfContractInfo] for target in ctx.attr.contracts]
    persistent_schemas = [target[OvfPerContractInfo] for target in ctx.attr.persistent_schemas]
    model = ctx.attr.model[OvfApplicationModelInfo]
    output = ctx.outputs.bundle
    arguments = ctx.actions.args()
    arguments.add("package")
    arguments.add("--name", ctx.attr.application_name)
    arguments.add("--executable", ctx.executable.application)
    arguments.add("--install-path", ctx.attr.install_path)
    arguments.add("--application-model", model.model)
    arguments.add("--application-deployment", ctx.file.deployment)
    for contract in contracts:
        arguments.add("--contract", contract.ir)
    for schema in persistent_schemas:
        arguments.add("--persistent-schema", schema.ir)
    arguments.add("--output", output)
    inputs = [ctx.executable.application, model.model, ctx.file.deployment]
    inputs.extend([contract.ir for contract in contracts])
    inputs.extend([schema.ir for schema in persistent_schemas])
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
        "contracts": attr.label_list(mandatory = True, providers = [OvfContractInfo]),
        "deployment": attr.label(mandatory = True, allow_single_file = [".cue"]),
        "model": attr.label(mandatory = True, providers = [OvfApplicationModelInfo]),
        "persistent_schemas": attr.label_list(providers = [OvfPerContractInfo]),
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
    model = ctx.attr.model[OvfApplicationModelInfo] if ctx.attr.model else None
    files = [ctx.file.deployment, ctx.executable.application]
    if model:
        files.append(model.model)
    return [
        DefaultInfo(files = depset(files)),
        OvfApplicationInfo(
            deployment = ctx.file.deployment,
            executable = ctx.executable.application,
            executable_target = ctx.attr.executable_target,
            install_path = ctx.attr.install_path,
            name = ctx.attr.application_name,
            model = model.model if model else None,
            contracts = [
                struct(name = name, info = target[OvfContractInfo])
                for name, target in zip(ctx.attr.interface_names, ctx.attr.contracts)
            ],
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
        "application_name": attr.string(),
        "model": attr.label(providers = [OvfApplicationModelInfo]),
        "contracts": attr.label_list(providers = [OvfContractInfo]),
        "interface_names": attr.string_list(),
    },
    doc = "Exports an application's deployment and executable to system integration.",
)

def _application_model_impl(ctx):
    model = ctx.outputs.model
    header = ctx.outputs.header
    arguments = ctx.actions.args()
    arguments.add("application-model")
    arguments.add("--cue", ctx.file._cue)
    arguments.add("--deployment", ctx.file.deployment)
    for name, target in zip(ctx.attr.interface_names, ctx.attr.contracts):
        arguments.add("--interface", name + "=" + target[OvfContractInfo].ir.path)
    arguments.add("--output", model)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset([ctx.file.deployment] + [target[OvfContractInfo].ir for target in ctx.attr.contracts]),
        tools = [ctx.file._cue],
        outputs = [model],
        mnemonic = "OvfApplicationModel",
    )
    codegen_arguments = ctx.actions.args()
    codegen_arguments.add("application-cpp")
    codegen_arguments.add("--model", model)
    codegen_arguments.add("--output", header)
    ctx.actions.run(
        executable = ctx.executable._codegen,
        arguments = [codegen_arguments],
        inputs = [model],
        outputs = [header],
        mnemonic = "OvfApplicationCpp",
    )
    return [
        DefaultInfo(files = depset([model, header])),
        OvfApplicationModelInfo(model = model, header = header),
        OutputGroupInfo(header = depset([header])),
    ]

ovf_application_model = rule(
    implementation = _application_model_impl,
    attrs = {
        "deployment": attr.label(mandatory = True, allow_single_file = [".cue"]),
        "contracts": attr.label_list(mandatory = True, providers = [OvfContractInfo]),
        "interface_names": attr.string_list(mandatory = True),
        "_builder": attr.label(default = Label("//tools:ovf_build"), executable = True, cfg = "exec"),
        "_codegen": attr.label(default = Label("//codegen:ovf_codegen"), executable = True, cfg = "exec"),
        "_cue": attr.label(default = Label("//bazel/host_tools:cue"), allow_single_file = True, cfg = "exec"),
    },
    outputs = {"model": "%{name}.application.json", "header": "generated/%{name}/ovf_application.hpp"},
    doc = "Validates portable application communication intent and generates its facade.",
)

def _ovf_cc_application_impl(
        name,
        srcs,
        interfaces,
        interface_names,
        deployment,
        application_name = None,
        execution_install_path = None,
        hdrs = [],
        deps = [],
        generated_artifacts = [],
        persistent_schemas = [],
        copts = [],
        defines = [],
        data = [],
        tags = [],
        visibility = None,
        **kwargs):
    """Composes reusable interfaces into one generated application dependency.

    Args:
      name: Application target name.
      application_name: Stable deployment name, independent of the Bazel target variant.
      srcs: C++ source files owned by the application.
      interfaces: Reusable interface targets composed into the process.
      interface_names: Stable logical names corresponding to interfaces.
      deployment: The application's CUE deployment document.
      hdrs: Application-owned headers.
      deps: Additional C++ dependencies.
      generated_artifacts: Cluster artifacts generated for this application.
      persistent_schemas: Persistent schemas packaged with the application.
      copts: Additional compiler options.
      defines: Additional preprocessor definitions.
      data: Runtime data files.
      tags: Bazel tags propagated to generated targets.
      execution_install_path: Executable path relative to the deployed filesystem root.
      visibility: Visibility of public targets.
      **kwargs: Additional attributes forwarded to the application binary.
    """
    facade_rule = name + "_application_model"
    ovf_application_model(
        name = facade_rule,
        deployment = deployment,
        contracts = interfaces,
        interface_names = interface_names,
        visibility = ["//visibility:private"],
    )
    artifacts = interfaces + generated_artifacts + [":" + facade_rule]
    cc_library(
        name = name + "_application_api",
        hdrs = [":" + facade_rule],
        includes = ["generated/" + facade_rule],
        deps = [Label("//com:api")],
        visibility = ["//visibility:private"],
    )
    cc_binary(
        name = name,
        srcs = srcs + hdrs,
        deps = interfaces + [":" + name + "_application_api"] + deps,
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
        contracts = interfaces,
        persistent_schemas = persistent_schemas,
        deployment = deployment,
        model = ":" + facade_rule,
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
        persistent_schemas = [],
        logging = False,
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

    Reusable Smithy interface and persistent-schema targets are composed with
    the application's one CUE deployment. Logging and persistence facades are
    generated internally rather than wired through companion application rules.

    Args:
      name: Application target name.
      application_name: Stable deployment name, independent of the Bazel target variant.
      srcs: C++ source files owned by the application.
      interfaces: Public OVF interface targets consumed by the application.
      deployment: The application's single CUE deployment model.
      persistent_schemas: Reusable persistent-schema targets used by the application.
      logging: Whether to generate the logging deployment facade.
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
    names = []
    for target in interfaces:
        interface_name = str(target).split(":")[-1].split("/")[-1]
        if interface_name in names:
            fail("duplicate interface target name: " + interface_name)
        names.append(interface_name)
    generated_deps = [Label("//exec:application_api"), Label("//app:app")]
    generated_artifacts = []
    if logging:
        logging_target = name + "_logging"
        ovf_internal_log_facade(
            name = logging_target,
            deployment = deployment,
            visibility = ["//visibility:private"],
        )
        generated_deps.append(":" + logging_target)
        generated_artifacts.append(":" + logging_target)
    if persistent_schemas:
        persistence_target = name + "_persistence"
        namespace_name = (application_name or name).replace("-", "_")
        ovf_internal_persistence_facade(
            name = persistence_target,
            cpp_namespace = "ovf::deployment::" + namespace_name,
            deployment = deployment,
            schemas = persistent_schemas,
            visibility = ["//visibility:private"],
        )
        generated_deps.extend([":" + persistence_target] + persistent_schemas)
        generated_artifacts.append(":" + persistence_target)
    _ovf_cc_application_impl(
        name = name,
        srcs = srcs,
        interfaces = interfaces,
        interface_names = names,
        deployment = deployment,
        application_name = application_name,
        execution_install_path = execution_install_path,
        hdrs = hdrs,
        deps = generated_deps + deps,
        generated_artifacts = generated_artifacts + persistent_schemas,
        persistent_schemas = persistent_schemas,
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
        application_name = application_name or name,
        model = ":" + name + "_application_model",
        contracts = interfaces,
        interface_names = names,
        executable_target = ":" + name,
        install_path = execution_install_path or "opt/%s/bin/%s" % (
            application_name or name,
            application_name or name,
        ),
        visibility = visibility,
    )
