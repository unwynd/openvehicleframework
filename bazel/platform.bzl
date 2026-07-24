# SPDX-License-Identifier: Apache-2.0

"""Rules for independently deployable OVF platform middleware bundles."""

def _vsomeip_platform_bundle_impl(ctx):
    runtime = ctx.files.framework + ctx.files.vsomeip
    arguments = ctx.actions.args()
    arguments.add("package-vsomeip-platform")
    arguments.add_all(runtime, before_each = "--runtime")
    arguments.add("--configuration", ctx.file.configuration)
    arguments.add("--service-unit", ctx.file.service_unit)
    arguments.add("--framework-license", ctx.file.framework_license)
    arguments.add("--vsomeip-license", ctx.file.vsomeip_license)
    arguments.add("--boost-license", ctx.file.boost_license)
    arguments.add("--output", ctx.outputs.bundle)
    inputs = runtime + [
        ctx.file.configuration,
        ctx.file.service_unit,
        ctx.file.framework_license,
        ctx.file.vsomeip_license,
        ctx.file.boost_license,
    ]
    ctx.actions.run(
        executable = ctx.executable._builder,
        arguments = [arguments],
        inputs = depset(inputs),
        outputs = [ctx.outputs.bundle],
        mnemonic = "OvfVsomeipPlatformPackage",
        progress_message = "Packaging vSomeIP platform middleware %{label}",
    )
    return [DefaultInfo(files = depset([ctx.outputs.bundle]))]

ovf_vsomeip_platform_bundle = rule(
    implementation = _vsomeip_platform_bundle_impl,
    attrs = {
        "boost_license": attr.label(allow_single_file = True, mandatory = True),
        "configuration": attr.label(allow_single_file = [".json"], mandatory = True),
        "framework": attr.label_list(allow_files = True, mandatory = True),
        "framework_license": attr.label(allow_single_file = True, mandatory = True),
        "service_unit": attr.label(allow_single_file = True, mandatory = True),
        "vsomeip": attr.label(mandatory = True),
        "vsomeip_license": attr.label(allow_single_file = True, mandatory = True),
        "_builder": attr.label(
            default = Label("//tools:ovf_build"),
            executable = True,
            cfg = "exec",
        ),
    },
    outputs = {"bundle": "%{name}.tar"},
    doc = "Creates a deterministic vSomeIP middleware filesystem bundle.",
)
