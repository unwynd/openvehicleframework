# Deployment support data

This directory contains framework-owned deployment support data:

- `profiles/`: provider capability and limit declarations;
- `schema/`: the typed CUE deployment schema and provider-profile schema;
- `testdata/`: focused positive or negative validator fixtures.

Application deployment inputs do not live here. Their canonical CUE files are
stored beside the relevant application under `examples/`. Resolved deployment
IR, plans, validation reports, C++ facades, and native mapping strings are
generated build artifacts and are not committed.

Provider mappings are generated from interface IR, application instance intent,
and platform policy. Applications do not author native identifiers, names,
buffer layouts, or resource mappings. Interface packages under `contracts/`
contain no deployment or transport data.

Provider selection and platform-wide policy belong under `platform/`. These
settings are independent of any particular service contract.
