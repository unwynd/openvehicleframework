# SPDX-License-Identifier: Apache-2.0

"""Public Bazel API for validated execution deployment artifacts."""

OvfExecutionDeploymentInfo = provider(
    doc = "Validated execution model and generated supervisor artifacts.",
    fields = {
        "backend_config": "Generated execution backend configuration.",
        "ir": "Canonical execution model JSON.",
        "manifest": "Integrity and generation manifest.",
        "services": "Generated dinit service-description directory.",
    },
)

def _exec_deployment_impl(ctx):
    execution_ir = ctx.actions.declare_file(ctx.label.name + ".execution.json")
    backend_config = ctx.actions.declare_file(ctx.label.name + ".backend.json")
    metadata = ctx.actions.declare_file(ctx.label.name + ".metadata.json")
    manifest = ctx.actions.declare_file(ctx.label.name + ".manifest.json")
    services = ctx.actions.declare_directory(ctx.label.name + ".dinit")
    arguments = ctx.actions.args()
    arguments.add("execution-deployment")
    arguments.add("--smithy", ctx.file._smithy)
    arguments.add("--model", ctx.file._model)
    arguments.add("--cue", ctx.file._cue)
    arguments.add("--schema", ctx.file._schema)
    arguments.add("--deployment", ctx.file.deployment)
    arguments.add("--platform", ctx.file.platform)
    arguments.add("--execution-ir", execution_ir)
    arguments.add("--backend-config", backend_config)
    arguments.add("--metadata", metadata)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = [
            ctx.file._model,
            ctx.file._smithy,
            ctx.file._schema,
            ctx.file._cue,
            ctx.file.deployment,
            ctx.file.platform,
        ],
        tools = ctx.files._smithy_runtime,
        outputs = [
            execution_ir,
            backend_config,
            metadata,
        ],
        mnemonic = "OvfExecutionDeployment",
        progress_message = "Generating execution deployment %{label}",
    )
    codegen_arguments = ctx.actions.args()
    codegen_arguments.add("execution-dinit")
    codegen_arguments.add("--model", execution_ir)
    codegen_arguments.add("--output", services.path)
    ctx.actions.run(
        executable = ctx.executable._codegen,
        arguments = [codegen_arguments],
        inputs = [execution_ir],
        outputs = [services],
        mnemonic = "OvfExecutionDinitCodegen",
        progress_message = "Rendering dinit deployment %{label}",
    )
    manifest_arguments = ctx.actions.args()
    manifest_arguments.add("execution-manifest")
    manifest_arguments.add("--metadata", metadata)
    manifest_arguments.add("--services", services.path)
    manifest_arguments.add("--manifest", manifest)
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [manifest_arguments],
        inputs = [
            metadata,
            services,
        ],
        outputs = [manifest],
        mnemonic = "OvfExecutionManifest",
        progress_message = "Finalizing execution manifest %{label}",
    )
    return [
        DefaultInfo(files = depset([
            execution_ir,
            backend_config,
            services,
            manifest,
        ])),
        OvfExecutionDeploymentInfo(
            backend_config = backend_config,
            ir = execution_ir,
            manifest = manifest,
            services = services,
        ),
        OutputGroupInfo(
            backend_config = depset([backend_config]),
            ir = depset([execution_ir]),
            manifest = depset([manifest]),
            services = depset([services]),
        ),
    ]

ovf_exec_deployment = rule(
    implementation = _exec_deployment_impl,
    attrs = {
        "deployment": attr.label(
            mandatory = True,
            allow_single_file = [".cue"],
            doc = "Platform-owned application and execution-domain allocation.",
        ),
        "platform": attr.label(
            mandatory = True,
            allow_single_file = [".cue"],
            doc = "Execution backend, persistence, and coordinator platform policy.",
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
        "_codegen": attr.label(
            default = Label("//codegen:ovf_codegen"),
            executable = True,
            cfg = "exec",
        ),
        "_model": attr.label(
            default = Label("//exec/model:execution.smithy"),
            allow_single_file = True,
        ),
        "_schema": attr.label(
            default = Label("//exec/deployment/schema:deployment.cue"),
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
    doc = "Validates Smithy+CUE execution deployment and generates runtime artifacts.",
)
