# Building applications with Bazel

Applications use the public `ovf_cc_application` macro. The application owns
its source, private headers, contract IDL and deployment intent; the framework
owns the hermetic compilers and generated-artifact contract.

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

Application code includes only the generated contract and public communication
API:

```cpp
#include "radar_app.ovf.hpp"
#include "radar_app_deployment.hpp"
```

The generated deployment facade loads the selected provider plugin and exposes
the service route without placing provider names or native identifiers in
application source.

## Generated targets

For an application named `radar_app`, the macro creates:

| Target | Purpose |
|---|---|
| `:radar_app` | C++ executable, dynamically linked to the framework API |
| `:radar_app_contract` | Generated C++ contract library |
| `:radar_app_contract_ir` | Canonical IR JSON |
| `:radar_app_contract_metadata` | Contract fingerprint and generation metadata |
| `:radar_app_deployment_api` | Generated transport-neutral runtime and route facade |
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

It intentionally contains no framework runtime or provider implementation.
The manifest records required provider profiles and content digests. Framework
delivery is a separate artifact:

```text
@openvehicleframework//com:middleware_runtime
@openvehicleframework//com:middleware_inproc
@openvehicleframework//com:vsomeip_plugin
@openvehicleframework//com/transports/iceoryx2:plugin
```

The application is linked against `libovf_com` dynamically. Transport
implementations remain behind the versioned provider ABI and are selected by
the validated deployment plan, not by application dependencies or includes.

## Public lower-level rules

Advanced integrations may use `ovf_contract`, `ovf_deployment` and
`ovf_application_package` directly. Their providers and output groups are
stable build-graph boundaries; applications should prefer
`ovf_cc_application` unless they need to integrate with another language or
packaging system.

Deployments that share a vSomeIP network use
`ovf_vsomeip_configuration`. It combines their validated plans and generates
the provider configuration; the application does not author or carry a second
configuration model.
