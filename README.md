# OpenVehicleFramework

OpenVehicleFramework is an early-stage, modular vehicle application framework.
Its first component, `ovf::com`, explores a typed service communication API
that is independent of the underlying IPC or network middleware.

> [!WARNING]
> Version 0.0.1 is experimental. It is not production-ready, safety-qualified,
> or intended for use in a vehicle. APIs, generated artifacts, deployment
> formats, and binary interfaces may change without compatibility guarantees.

## Current scope

The repository currently contains:

- an explicit communication runtime with static and dynamic transport
  registration;
- a versioned C transport boundary usable by C++ and Rust implementations;
- Smithy-based service definitions and template-driven Rust C++ generation;
- deployment schema validation and deterministic application packaging;
- an in-process reference transport;
- experimental iceoryx2 and vSomeIP transport integrations;
- transport conformance, interoperability, and example application tests.

DDS is represented in the deployment model, but a DDS transport implementation
is not included in this release.

## Architecture

```text
Application
    │
Generated C++ contract API
    │
Transport-neutral runtime
    │
Versioned C transport boundary
    ├── in-process reference transport
    ├── experimental iceoryx2 transport
    └── experimental vSomeIP transport
```

Service contracts are separate from deployment configuration. Applications use
generated types, generated deployment facades, and `ovf::com` APIs. Deployment
selects runtime-loaded provider plugins and generates their configuration.

See the [communication architecture](docs/com-architecture.md) for the public
design principles and provider model.

## Building

The primary build uses Bazel 8.2.1. Bazel downloads the declared build
dependencies and toolchains; a host installation of the project dependencies
is not required.

```sh
bazel test //:all_tests
bazel test --config=strict //:all_tests
bazel run //quality:check
```

Build the example application and its deterministic target bundle with:

```sh
bazel build //examples/radar_app:radar_app
bazel build //examples/radar_app:radar_app_bundle
```

On Linux, run the generated service/client vSomeIP example as two processes:

```sh
bazel test //tests/integration/vsomeip:radar_two_process_test
```

See the [radar vSomeIP example](examples/radar/README.md) for the
interactive service and client commands.

CMake is available as a compatibility build for the transport-neutral runtime,
the in-process transport, and their tests.

The [repository quality gate](quality/README.md) checks source formatting,
Starlark build files, SPDX headers, syntax, repository hygiene, and strict
compiler warnings using Bazel-managed tools.

For native or QEMU-emulated Linux validation, the
[Linux development lab](lab/README.md) provides a mountable multi-architecture
image with persistent build cache, structured logs, optional key-only SSH,
network diagnostics, and exportable root filesystems.

## Building an application

The public `ovf_cc_application` Bazel macro accepts application sources,
headers, Smithy IDL, and deployment configuration:

```starlark
load("@openvehicleframework//bazel:application.bzl", "ovf_cc_application")

ovf_cc_application(
    name = "radar_app",
    srcs = ["main.cpp"],
    hdrs = ["radar_logic.hpp"],
    idl = ["radar.smithy"],
    deployment = "radar-deployment.json",
)
```

It creates the application executable, generated contract library, canonical
IR, deployment plan, validation report, and target bundle. Middleware binaries
are delivered separately from the application bundle.

See [Building applications with Bazel](docs/building-applications.md) for the
generated targets and shipping boundary.

## Repository layout

```text
bazel/          public build rules, toolchains, and build tests
codegen/        Rust generator and versioned MiniJinja templates
com/            runtime, public API, deployments, transports, and tests
contracts/      authored service contracts
docs/           public architecture and application documentation
examples/       example applications
integration/    independent consumer build fixture
lab/            Linux validation image, rootfs export, and debug entry point
quality/        formatting, licensing, and repository hygiene gate
tools/          model compilation, validation, and packaging tools
```

## Versioning

The project uses semantic versioning. During the `0.x` series, source, binary,
model, deployment, and generated-code compatibility are not guaranteed between
releases. Compatibility commitments will be defined before a stable 1.0
release.

## Third-party dependencies

Key transport-related dependencies are retrieved under their own licenses:

| Dependency | Purpose | License |
|---|---|---|
| [vSomeIP](https://github.com/COVESA/vsomeip) | SOME/IP transport | [MPL-2.0](https://www.mozilla.org/MPL/2.0/) |
| [iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2) | shared-memory transport | Apache-2.0 OR MIT |
| [Boost](https://www.boost.org/) | vSomeIP dependency | [Boost Software License 1.0](https://www.boost.org/LICENSE_1_0.txt) |
| [Buildifier](https://github.com/bazelbuild/buildtools) | Starlark formatting and linting | Apache-2.0 |

These dependencies are not relicensed under the project's Apache-2.0 license.
Future binary distributions must include the license and notice material
required by the exact dependency versions they contain.

## License

OpenVehicleFramework is licensed under the
[Apache License 2.0](LICENSE).
