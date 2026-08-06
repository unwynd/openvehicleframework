# Communication performance baselines

This benchmark measures the neutral transport ABI with identical workloads for
iceoryx2 and vSomeIP:

- 100 warm-up method calls;
- 500 sequential 256-byte echo calls for round-trip latency;
- 2,000 256-byte echo calls in windows of 32 for request throughput;
- a burst of 2,000 reliable 256-byte events for publish-to-callback throughput.

Every response and event is validated. A missing, malformed, failed, or timed
out transfer fails the run instead of producing a performance result.

Run on Linux with:

```sh
bazel run --config=strict //benchmarks/com:run
```

Results are JSON intended for collection by CI. Absolute values from Docker
Desktop include virtualization and scheduler noise and are development
baselines, not target-hardware claims. Generated codec cost, CPU consumption,
memory footprint, allocation count, and copy count require separate workloads.
