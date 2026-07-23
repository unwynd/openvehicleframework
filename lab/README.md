# Linux validation lab

The lab is a multi-architecture Ubuntu development image for fast Linux
validation and post-failure debugging. It contains a fallback source snapshot,
but a bind-mounted `/workspace` replaces that snapshot without rebuilding the
image. Bazel state and logs live on separate writable volumes.

The entry point never changes ownership of an existing bind-mounted source
tree. On native Linux, ensure user `1000` can read it and can write any desired
generated files; mount the source read-only when only validation is needed.

## Build

Build and load the host-relevant Linux image:

```sh
docker buildx bake -f lab/docker-bake.hcl lab-arm64 --load
```

Use `lab-amd64` on an x86-64 host. Selecting the other architecture exercises
the image through the Docker host's QEMU/binfmt support. The `validated-amd64`
and `validated-arm64` targets additionally compile the test suite into the
image cache.

Export a validated plain root filesystem for a VM, container, or overlayfs
lower directory:

```sh
docker buildx bake -f lab/docker-bake.hcl rootfs-arm64
```

The result is written below `out/linux-rootfs-arm64`. Source, Bazel cache, and
logs remain writable mount points and can be supplied as overlay upper layers
or ordinary bind mounts.

## Run tests

```sh
docker run --rm \
  --name ovf-lab \
  --cap-add NET_RAW \
  -v "$PWD:/workspace" \
  -v ovf-bazel-cache:/var/cache/ovf/bazel \
  -v "$PWD/out/lab-logs:/var/log/ovf" \
  openvehicleframework/dev-lab:local
```

Docker networking permits outbound access by default. Use `--network host`
when a native Linux host must expose multicast or middleware traffic directly;
on Docker Desktop, publish the specific ports instead.

The log volume receives:

- `console.log`;
- `environment.json`;
- Bazel build-event JSON;
- copied Bazel test logs and XML results.

## SSH and debugging

SSH is disabled unless an authorized-keys file is explicitly mounted:

```sh
docker run --rm -d \
  --name ovf-lab \
  --cap-add NET_ADMIN --cap-add NET_RAW --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -p 2222:22 \
  -e OVF_LAB_HOLD=1 \
  -v "$PWD:/workspace" \
  -v "$HOME/.ssh/id_ed25519.pub:/run/ovf/authorized_keys:ro" \
  -v ovf-bazel-cache:/var/cache/ovf/bazel \
  -v "$PWD/out/lab-logs:/var/log/ovf" \
  openvehicleframework/dev-lab:local

ssh -p 2222 ovf@localhost
```

The image includes gdb, strace, tcpdump, iproute2, lsof, procps, netcat, tmux,
Vim, CMake, Ninja, Clang, and common network diagnostics. `docker exec` remains
available when SSH is unnecessary.
