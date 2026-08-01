# `ovf::com` architecture

`ovf::com` is an experimental, transport-neutral service communication API.
Its interfaces and wire contracts may change. The project does not currently
make production-readiness or functional-safety claims.

## Structure

```text
Application
    │
Generated C++ proxy, skeleton, and offer API
    │
Discovery, route selection, and binding runtime
    │
Versioned C transport ABI
    ├── in-process reference transport
    ├── iceoryx2 shared-memory transport
    ├── vSomeIP transport (Linux)
    └── Cyclone DDS transport (planned)
```

The application-facing API deals in generated service types. Transport
implementations exchange byte payloads through a C ABI that avoids C++ object
layouts, exceptions, RTTI, and STL types at the provider boundary. A transport
may be implemented in any language that can implement this ABI.

## Runtime mechanics

A `Runtime` owns its transport instances. Generated deployment facades ask the
runtime to load the selected providers; application source does not include
provider headers or name native identifiers. Start occurs in registration
order and rolls back on failure; shutdown occurs in reverse order. Static
registration and dynamically loaded plugins converge on the same versioned
factory interface, and neither requires a global registry or static
constructor.

Generated APIs provide the normal application lifecycle:

```text
Application facade ── load transports once and own runtime lifetime
        │
        ├── Proxy::Find(instance, timeout) ── discover, select, connect
        └── Skeleton::OfferService(instance) ── offer and own server binding
```

The lower-level discovery, connection, offer, and route APIs remain available
for infrastructure and asynchronous integrations. Discovery combines provider
observations into service routes. Route priority,
provider name, instance identifier, and route epoch give selection a stable
order. A proxy retains the selected binding for its lifetime. Availability
changes do not silently move an existing proxy, retry a request, or alter its
delivery semantics.

## Contract and deployment separation

Smithy service models define types, events, methods, fields, errors, and stable
UUID identities. The generator produces codecs and the typed C++ client/server
surface. Application-owned CUE deployment intent defines instances, roles,
and optional requirements without selecting native identities. Platform-owned
Independent binding CUE targets each select one provider profile and supply its
policy and extensions. System integration composes only the providers
needed by that target. The compiler derives provider-native identities,
resource mappings, and routes from the interface IR, instance declaration, and
composed binding policy.

The deployment compiler resolves shape names to stable IDs and adds the
contract fingerprint. Application authors never copy those generated
identities or duplicate deployment intent for each transport. Changing a
binding policy does not change or rebuild the application API or executable.
Generated application code contains stable service-and-instance selectors;
concrete routes and transport registrations are loaded from the system-installed
runtime deployment.

The model distinguishes bounded and unbounded data. Providers advertise
capabilities and resource limits and may reject endpoints that cannot meet a
deployment. Zero-copy loans remain owned by the provider that created them;
the runtime does not assume that a loan can cross a transport boundary.

Provider configuration is a platform artifact. The shared vSomeIP bootstrap
contains routing and service-discovery settings but no application or service
inventory. Validated per-application runtime deployments carry provider mappings
and are installed by system integration independently from both middleware and
application bundles. Application authors do not maintain a
second native middleware configuration.

## Transport coverage

- The in-process transport is the reference implementation for discovery,
  events, methods, deadlines, cancellation, and shutdown behavior.
- The iceoryx2 transport supports discovery, variable-size events, native
  publisher/subscriber loans, scatter/gather publication, request/response
  methods, application errors, deadlines, cancellation, and ordered delivery.
  Provider availability uses a binding-owned discovery service; application
  payloads use iceoryx2's native publish/subscribe and request/response
  messaging patterns.
- The vSomeIP transport supports discovery, events, and request/response on
  Linux. Its native peer test is platform-gated.
- A Cyclone DDS transport is planned but is not implemented.

The generated lifecycle is exercised end to end with the in-process and
iceoryx2 providers. The iceoryx2 two-process test uses the same transport-neutral
radar service and client sources and verifies discovery, methods, application
errors, field reads, event delivery, and field notifications.

## Stability boundary

The C transport ABI is versioned, but ABI compatibility is not promised before
an explicit version-1 readiness gate. Generated C++ APIs and deployment
artifacts are likewise experimental. Capability validation, bounded resource
behavior, deterministic code generation, and explicit teardown are design
inputs; they are not evidence of a safety-qualified implementation.
