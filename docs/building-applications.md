# Building applications with Bazel

Applications use the public `ovf_cc_application` macro. The application owns
its source, private headers, and CUE deployment intent. Interface packages
export Smithy contracts as Bazel targets; the framework owns the hermetic
compilers and generated-artifact contract.

```starlark
load("@openvehicleframework//bazel:application.bzl", "ovf_cc_application")

ovf_cc_application(
    name = "radar_app",
    srcs = ["main.cpp"],
    hdrs = ["radar_logic.hpp"],
    interfaces = ["//contracts/radar"],
    deployment = "radar.deployment.cue",
    platform = "//platform:vsomeip",
)
```

Application code includes generated contract types and one application facade:

```cpp
#include "radar/ovf_contract.hpp"
#include "ovf_application.hpp"

auto application = ovf::app::CreateRuntime("radar-app");
auto proxy = RadarServiceProxy::Find(
    application.get(), ovf::app::radar(), std::chrono::seconds(10));
```

The application facade loads each selected provider once, owns runtime
start/stop through RAII, and exposes named instance routes. Generated proxies
perform bounded discovery and connection. Generated skeletons offer services
without exposing raw bindings.

## Generated targets

For an application named `radar_app`, the macro creates:

| Target | Purpose |
|---|---|
| `:radar_app` | C++ executable, dynamically linked to the framework API |
| `:radar_app_application_api` | Generated runtime and named-instance facade |
| `:radar_app_artifacts` | IDL, headers, generated code, IR, deployment plan and validation report |
| `:radar_app_bundle` | Deterministic target tar containing the application and target configuration |

The build fails before C++ compilation when the contract is invalid, and fails
before packaging when deployment capabilities, bounds, identifiers or provider
mappings are invalid.

## Shipping boundary

The application tar contains:

```text
bin/<application>
etc/ovf/<application>/deployment.json
etc/ovf/<application>/plan.json
share/ovf/<application>/contract.ovf-ir.json
share/ovf/<application>/deployment-validation.json
share/ovf/<application>/manifest.json
```

The same `ovf_cc_application` API accepts one or many interface targets. Every
application owns one deployment model containing all of its logical provider
and consumer roles. Generated headers are namespaced by interface target name,
and the application bundle carries contract, deployment, plan, and validation
artifacts for every role.

It intentionally contains no framework runtime or provider implementation.
The manifest records required provider profiles and content digests. Framework
delivery is a separate artifact:

```text
@openvehicleframework//com:middleware_runtime
@openvehicleframework//com:middleware_inproc
@openvehicleframework//com:vsomeip_plugin
@openvehicleframework//com:vsomeip_platform_bundle
@openvehicleframework//com/transports/iceoryx2:plugin
```

The application is linked against `libovf_com` dynamically. Transport
implementations remain behind the versioned provider ABI and are selected by
the validated deployment plan, not by application dependencies or includes.

## Deployment source ownership

Authored CUE deployment intent lives with the application that consumes it:

```text
examples/<application>/<role>/deployment.cue
platform/providers/<provider>.cue
```

Smithy interface targets define communication contracts. Application-owned CUE names the
interface and logical instance and declares the provider or consumer role.
Platform-owned CUE selects the communication implementation and supplies
platform policy and extensions. The compiler generates provider-native names,
identifiers, resource mappings, stable instance identity, and the canonical
deployment plan. `contracts/` contains only interface definitions and
associated interface build metadata.

CUE sources reference qualified Smithy shape names. The compiler resolves
stable service and element IDs and injects the contract fingerprint. Native
transport mappings are generated and are never authored or duplicated by
applications.

These files are the canonical executable examples and are used directly by
their Bazel application targets and deployment tests. `com/deployment` contains
only provider profiles, schemas, and focused validator test data. It does not
contain a second copy of application deployments.

Resolved deployment JSON, canonical plan JSON, validation reports, generated
C++ facades, and native mapping strings are build outputs. They are not authored
or committed.

## Public lower-level rules

Advanced integrations may use `ovf_contract`, `ovf_deployment` and
`ovf_application_package` directly. Their providers and output groups are
stable build-graph boundaries; applications should prefer
`ovf_cc_application` unless they need to integrate with another language or
packaging system.

The shared vSomeIP routing and service-discovery bootstrap is stored as the
transport-specific platform configuration
`com/transports/vsomeip/config/platform.json`. It is independent of
application deployments and contains no application or service inventory.
Each target platform may replace the unicast address while retaining this
transport-owned structure. Network-facing provider endpoints will be
registered dynamically after the planned vSomeIP endpoint-registration
extension is available.

The vSomeIP platform bundle has a deterministic root-filesystem layout:

```text
etc/vsomeip.json
usr/bin/routingmanagerd
usr/lib/libovf_com.so
usr/lib/libvsomeip3*.so*
usr/lib/ovf/providers/libovf_com_provider_vsomeip.so
usr/lib/systemd/system/ovf-vsomeip-routing.service
usr/share/licenses/{openvehicleframework,vsomeip,boost}/...
usr/share/ovf/platform/vsomeip/manifest.json
```

It contains no application executable or deployment plan. Application bundles
can be installed, restarted, or replaced independently while the routing
manager remains active.
