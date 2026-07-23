# QNX SDP toolchain contract

QNX targets use an explicitly provisioned, licensed SDP. The SDK is never
downloaded from an unlicensed public location and is never inferred from the
shell `PATH`.

The future registered toolchain accepts one repository path supplied by CI or
the developer environment and validates, before analysis:

- the exact supported SDP release and an organization-published fingerprint;
- `host/<host>/usr/bin/qcc` and `q++`;
- the target sysroot beneath `target/qnx*`;
- the requested target variant (`gcc_ntox86_64` or
  `gcc_ntoaarch64le` for the initial targets);
- compiler, linker, archiver, standard-library, and system-header inputs;
- license availability with a finite queue timeout.

The path and license are provisioning inputs. All flags, target constraints,
SDK-relative paths, and selected files are declared by the Bazel toolchain.
No QNX target may fall back to the host LLVM toolchain.

The public platform labels are `//bazel/platforms:qnx_x86_64` and
`//bazel/platforms:qnx_aarch64`. Registration of the actual compiler is gated
on access to the licensed SDP and must be qualified independently for each SDP
release used in production.
