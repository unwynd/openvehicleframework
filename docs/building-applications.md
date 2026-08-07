# Building applications with Bazel

Applications use reusable `ovf_interface` and `ovf_persistent_schema` targets
with the public `ovf_cc_application` macro. The application owns its source,
private headers, and one CUE deployment intent; the framework owns hermetic
compilers, cluster facade generation, and artifact wiring.

```starlark
load(
    "@openvehicleframework//bazel:application.bzl",
    "ovf_cc_application",
    "ovf_interface",
    "ovf_persistent_schema",
)

ovf_interface(
    name = "radar",
    srcs = ["radar.smithy"],
)

ovf_persistent_schema(
    name = "radar_state",
    srcs = ["radar-state.smithy"],
)

ovf_cc_application(
    name = "radar_app",
    srcs = ["main.cpp"],
    hdrs = ["radar_logic.hpp"],
    interfaces = [":radar"],
    deployment = "radar.deployment.cue",
    logging = True,
    persistent_schemas = [":radar_state"],
    crypto = True,
)
```

Communication interfaces and persistent schemas use the same bounded Smithy
data-type profile and stable member tags. Their generated envelopes and codecs
remain cluster-specific because network compatibility and durable-record
evolution have different requirements. `logging`, `persistent_schemas`, and
`crypto` opt the application into the corresponding facilities; the deployment
CUE must declare each facility that the target opts into, and vice versa.

The recommended entry point is `ovf::app::Run`, which composes exec, com, log,
persistence, and crypto startup and hands the body a `Context`:

```cpp
#include "ovf/app/run.hpp"
#include "ovf_application.hpp"
#include "ovf_logging.hpp"
#include "radar/ovf_contract.hpp"

using namespace ovf::log::literals;

int main() {
    return ovf::app::Run("radar-app", "radar.client", [](ovf::app::Context& ctx) {
        auto proxy = RadarServiceProxy::Find(ctx.com(), ovf::app::radar(),
                                             std::chrono::seconds(10));
        if (!proxy) return ovf::app::ExitCode::discovery_timeout;
        if (auto ready = ctx.ReportReady(); ready != ovf::app::ExitCode::ok) return ready;
        ctx.logger().Info("radar client ready", "instance"_field = "front-radar");
        return ctx.Run();
    });
}
```

`Context` exposes `com()`, `log()`, `logger()`, `per()`, and `crypto()`. The
last two return `nullptr` when the facility is not declared; a declared
facility that fails to initialize is fatal and prevents the body from running.
The generated `ovf_application.hpp` provides logical instance selectors such
as `ovf::app::radar()` and constants for every log event declared in the
deployment (for example `ovf::app::kEnvironmentModelReceived`).

Applications that need the flat facade can still use
`ovf::app::CreateRuntime`, which returns
`ovf::com::ApplicationRuntimeResult`:

```cpp
auto application = ovf::app::CreateRuntime("radar-app");
if (!application) {
    // application.error() carries the communication error and diagnostic.
    return 1;
}
auto proxy = RadarServiceProxy::Find(application.value().get(),
                                     ovf::app::radar(), std::chrono::seconds(10));
```

Generated proxies perform bounded discovery and connection; generated skeletons
offer services without exposing raw bindings. Method calls return a typed
`MethodOutcome<Value, ApplicationError>` with `.ok()`, `.is<T>()`, and
`.as<T>()` accessors, and `Operation<T>::get()` waits on the deadline captured
at submission.

## Generated targets

For an application named `radar_app`, the macro creates:

| Target | Purpose |
|---|---|
| `:radar_app` | C++ executable, dynamically linked to the framework API |
| `:radar_app_execution_deployment` | Application deployment and executable-target identity exported to system integration |
| `:radar_app_application_api` | Generated runtime and named-instance facade |
| `:radar_app_artifacts` | IDL, generated contract IR, application model, and generated code |
| `:radar_app_bundle` | Deterministic transport-neutral application tar |

The build fails before C++ compilation when the contract is invalid, and fails
before packaging when application intent cannot be matched to its contracts.
Provider capability and mapping validation occurs in system integration.

Applications declare only the need for crypto (`crypto: {}`); the platform
selects the provider through `OVF_CRYPTO_PROVIDER` and its provider path.
Persistence declares its stores in the CUE deployment; the generated
`ovf_persistence.hpp` emits per-store `Open<Store>` helpers and each record
type gets a `<Name>Persistent` wrapper with `Put(tx, value)` and `Get(tx)`
that use the deployment's bound internally.

## Shipping boundary

The application tar contains:

```text
opt/<application>/bin/<application>
share/ovf/<application>/application.json
share/ovf/<application>/deployment.cue
share/ovf/<application>/contract-<n>.ovf-ir.json
share/ovf/<application>/manifest.json
```

The same `ovf_cc_application` API accepts one or many interface targets. Every
application owns one deployment model containing all of its logical provider
and consumer roles. Generated headers are namespaced by interface target name,
and the application bundle carries portable contracts and intent for every role.

It intentionally contains no framework runtime or provider implementation.
The manifest records no required provider implementation and includes content digests. Framework
delivery is a separate artifact:

```text
@openvehicleframework//com:middleware_runtime
@openvehicleframework//com:middleware_inproc
@openvehicleframework//com:vsomeip_plugin
@openvehicleframework//com:vsomeip_platform_bundle
@openvehicleframework//com/transports/iceoryx2:plugin
```

The application is linked against `libovf_com` dynamically. Transport
implementations remain behind the versioned provider ABI. A system-owned
`ovf_communication_deployment` target composes independent communication
bindings, for example
`bindings = ["//com/deployment/bindings:iceoryx2.cue", "//com/deployment/bindings:vsomeip.cue"]`, and
installs the resulting runtime JSON separately from the application bundle.

## Deployment source ownership

Authored CUE deployment intent lives with the application that consumes it:

```text
examples/<application>/<role>/deployment.cue
com/deployment/bindings/<provider>.cue
```

Smithy interface targets define communication contracts. Application-owned CUE
has one `application` root. It names the application, declares communication
roles and logical transports, and defines lifecycle requirements such as
readiness and bounded startup, shutdown, and restart policy. It does not assign
runtime IDs, installation paths, dependencies, resources, execution domains,
or modes. The Bazel application target exports the built executable together
with its path relative to the deployed filesystem root. The default is
`opt/<application>/bin/<application>`; platform integration resolves that
relative path against the application filesystem mount point.
System-selected binding CUE chooses the communication implementation and
supplies binding policy and extensions. The compiler generates provider-native names,
identifiers, resource mappings, stable instance identity, and the canonical
deployment plan. Example interface subpackages contain only interface definitions and
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

System integration composes `*_execution_deployment` application targets with
an execution allocation target. The allocation defines one typed execution
unit graph containing managed applications, external services, one-shot
operations, mounts, and native supervisor services. It assigns runtime IDs,
dependencies, exclusive resources, bootstrap status, and domain/mode
membership. Execution-platform CUE supplies supervisor, persistence,
coordinator, application mount point, and target-specific commands for system
units. Symbolic unit names are resolved to stable numeric runtime references
only in generated execution IR.

Every non-bootstrap unit has explicit mode membership. Bootstrap units are
kept outside modes, may depend only on other bootstrap units, and are generated
as dependencies of the supervisor boot target. Generated dinit descriptions
repeat the validated hard dependency edges; startup is topological and
shutdown is reverse-topological while shared units remain retained.

The execution manifest binds every managed application name, Bazel target,
relative installation path, and executable SHA-256 digest. Target packaging
rejects missing, additional, path-mismatched, or digest-mismatched application
bundles. `ovf_exec_target_bundle` then produces a deterministic filesystem
layer containing application bundles, platform middleware, dinit,
`ovf-execd`, its backend plugin, generated supervisor descriptions, and the
validated execution artifacts. The generated `boot` service starts the
generated `ovf-execd` service; the daemon is no longer an undeclared external
bootstrap step.

For the example system:

```text
bazel build //examples/execution:target_filesystem
```

The resulting `target_filesystem.tar` is rooted at the target filesystem. It
is suitable as an image layer or for extraction below the platform-selected
mount root; application bundles remain independently replaceable inputs.

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
