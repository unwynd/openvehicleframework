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
- The iceoryx2 transport is daemonless: every process owns an iceoryx2 node and
  exchanges application data through native publish/subscribe and
  request/response services. Generated event, request, and response encoders
  write directly into provider-owned loans; older ABI providers use the bounded
  copy fallback.
- Each data service has a binding-owned event service used only for readiness
  notification. Its listener file descriptor is integrated with the transport
  reactor, so delivery does not depend on a sleep-and-poll loop. A bounded
  reactor timeout reconciles deadlines and abnormal peer state.
- Provider discovery uses a separate `<service>/__ovf_provider` event service.
  Current notifier count is the authoritative availability snapshot, while
  notifier-created, notifier-dropped, and notifier-dead events wake discovery
  watches. No discovery daemon or payload publisher heuristic is required.
- Event samples remain provider-owned until the synchronous application
  callback returns. The callback decodes directly from the shared-memory view;
  retaining that view beyond the callback is invalid. Request and response
  loans are consumed exactly once by send, publish, release, cancellation, or
  shutdown.
- Incoming method requests are dispatched as views over their active native
  request, and incoming native responses remain borrowed through the completion
  callback. The asynchronous C++ operation then performs one ownership copy so
  its result can safely outlive the transport callback. Typed decoding may copy
  fields into the generated value; the transport adds no intermediate staging
  copy.
- Deployment fixes publisher, subscriber, client, server, history, buffer, and
  outstanding-loan budgets. iceoryx2 services use static allocation and reject
  creation or traffic when these bounds cannot be honored; the binding does not
  silently degrade delivery guarantees.
- The vSomeIP transport supports discovery, events, and request/response on
  Linux. Its native peer test is platform-gated.
- A Cyclone DDS transport is planned but is not implemented.

The generated lifecycle is exercised end to end with the in-process and
iceoryx2 providers. The iceoryx2 two-process test uses the same transport-neutral
radar service and client sources and verifies discovery, methods, application
errors, field reads, event delivery, and field notifications.

## Stability boundary

The C transport ABI is versioned and extended append-only. Consumers validate
`struct_size` before reading optional function pointers and negotiate native
event, request, and response loans through capability bits. Older version-1
providers remain usable through the base-size boundary and copy fallback.
Compatibility is not promised before an explicit version-1 readiness gate.
Generated C++ APIs and deployment artifacts are likewise experimental.
Capability validation, bounded resource behavior, deterministic code
generation, and explicit teardown are design inputs; they are not evidence of
a safety-qualified implementation.
