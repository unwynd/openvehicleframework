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
    ├── iceoryx2 event transport
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

Service use follows an explicit lifecycle:

```text
Discover(candidate routes)
    │
    ├── receive availability changes
    └── select the deterministic preferred route
             │
             └── Connect(route) ── generated proxy

Offer(route)
    │
    └── generated service offer ── application skeleton
```

Discovery combines provider observations into service routes. Route priority,
provider name, instance identifier, and route epoch give selection a stable
order. A proxy retains the selected binding for its lifetime. Availability
changes do not silently move an existing proxy, retry a request, or alter its
delivery semantics.

## Contract and deployment separation

Service models define types, events, methods, fields, errors, and stable UUID
identities. The generator produces codecs and the typed C++ client/server
surface. Deployment data separately maps service instances and elements to
provider-native names, resource limits, and route priorities. Changing a
deployment mapping does not change the generated application API.

The model distinguishes bounded and unbounded data. Providers advertise
capabilities and resource limits and may reject endpoints that cannot meet a
deployment. Zero-copy loans remain owned by the provider that created them;
the runtime does not assume that a loan can cross a transport boundary.

Provider configuration is also a deployment artifact. For example, the
vSomeIP generator combines validated application plans into its application,
service, event, event-group, endpoint, routing, and discovery configuration.
Application authors do not maintain a second provider-specific configuration
file.

## Transport coverage

- The in-process transport is the reference implementation for discovery,
  events, methods, deadlines, cancellation, and shutdown behavior.
- The iceoryx2 transport supports native event exchange and provider loans.
  It does not currently implement discovery or request/response methods.
- The vSomeIP transport supports discovery, events, and request/response on
  Linux. Its native peer test is platform-gated.
- A Cyclone DDS transport is planned but is not implemented.

The generated lifecycle is exercised end to end with the in-process provider.
Provider conformance tests cover lower-level cancellation and teardown
semantics, while native peer tests cover the capabilities implemented by each
external middleware.

## Stability boundary

The C transport ABI is versioned, but ABI compatibility is not promised before
an explicit version-1 readiness gate. Generated C++ APIs and deployment
artifacts are likewise experimental. Capability validation, bounded resource
behavior, deterministic code generation, and explicit teardown are design
inputs; they are not evidence of a safety-qualified implementation.
