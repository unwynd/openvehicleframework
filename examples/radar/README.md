# Radar service and client over vSomeIP

This Linux-only example runs a generated `RadarService` service and client as
separate processes. It demonstrates:

- service offer and discovery;
- a successful method response;
- a typed application error;
- an event subscription and payload exchange;
- a field read and field notification.

The application sources use only the generated service API and the generated
deployment facade. Provider loading, native identifiers, routes, and provider
configuration are derived from the validated deployment files.

## Automated two-process run

```sh
bazel test //tests/integration/vsomeip:radar_two_process_test
```

The test starts `radar_service`, waits until it has offered the service, runs
`radar_client`, verifies all expected exchanges, and terminates the service.

## Run interactively

Build both applications:

```sh
bazel build //examples/radar:radar_service \
  //examples/radar:radar_client
```

Generate the shared provider configuration:

```sh
bazel build //examples/radar:provider_configuration
```

From the repository root, start the service:

```sh
OVF_COM_PROVIDER_PATH="<directory-containing-libovf_com_provider_vsomeip.so>" \
VSOMEIP_CONFIGURATION="$PWD/bazel-bin/examples/radar/provider_configuration.json" \
  bazel-bin/examples/radar/radar_service
```

In a second terminal, start the client with the same configuration:

```sh
OVF_COM_PROVIDER_PATH="<directory-containing-libovf_com_provider_vsomeip.so>" \
VSOMEIP_CONFIGURATION="$PWD/bazel-bin/examples/radar/provider_configuration.json" \
  bazel-bin/examples/radar/radar_client
```

The client exits successfully after printing:

```text
DISCOVERED
METHOD_OK
APPLICATION_ERROR_OK
FIELD_READ_OK
EVENT_OK
FIELD_NOTIFICATION_OK
```

Stop the service with `Ctrl-C`.
