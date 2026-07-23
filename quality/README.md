# Repository quality gate

Run the complete source and build-file gate with:

```sh
bazel run //quality:check
bazel test --config=strict //:all_tests
```

The first command uses Bazel-managed Buildifier and clang-format binaries. It
checks Starlark lint/formatting, C and C++ formatting, Apache-2.0 SPDX headers,
UTF-8, final newlines, whitespace, JSON and Python syntax, header conventions,
and repository naming policy. Rust formatting is enforced by
`//codegen:rustfmt_test` in the normal test suite.

JSON, Markdown, lock files, and generated templates are checked for hygiene but
do not require comment headers when their format cannot carry one.
