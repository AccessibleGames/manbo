# Development

## Builds

Use Conda `mlc` for Linux configuration and builds. Dependencies are listed in
`environment-linux.yml`: CMake, Ninja, Clang, Boost headers (1.86+), CLI11, fmt, GTest, libxml2,
nlohmann_json, SQLite, OpenSSL Crypto, and tl-expected. All application and
library-maintenance code is C++; tests use GTest and CMake.

| Preset | Output | Purpose |
|---|---|---|
| `linux-mlc` | `build/linux/debug/` | Application development |
| `linux-mlc-tests` | `build/linux/tests/` | Application and all checks |
| `linux-mlc-release` | `build/linux/release/` | Optimized application |
| `linux-mlc-library-only` | `build/linux/library-only/` | Libraries and consumer checks |

Use `cmake --preset NAME`, `cmake --build --preset NAME`, and, for test-enabled
presets, `ctest --preset NAME`. Refresh clangd's ignored root database with:

```sh
cmake --build --preset linux-mlc-tests --target compile-commands
```

Windows uses the corresponding `windows-*` presets, clang-cl, the MSVC SDK,
and vcpkg. In an x64 Visual Studio developer shell, set `VCPKG_ROOT` to the
vcpkg checkout, install the ports in `vcpkg.json`, then configure
`windows-debug`. `windows-manifest-debug` enables a project-local manifest
installation. Windows changes require a Windows runner for native validation.

## Code style

Use the root `.clang-format`, which uses options supported by clang-format 10
and newer. Layout can vary slightly across releases; the formatting contract
checks stable properties and idempotence.

- Two-space indentation and an 80-column target.
- Function names begin below return types. Wrapped argument lists use one item
  per line; short calls may remain compact.
- Put blank lines between functions and between distinct steps within a function.
- Use descriptive `snake_case` variables, `UpperCamelCase` functions,
  `kEnumValues`, and trailing underscores for private fields.
- Explain invariants, ownership, and non-obvious decisions in comments.
- Keep `.h` files beside their implementations, with unique header guards.
- Recoverable failures return `Result<T>` (`tl::expected`). Contain exceptions
  at backend and application boundaries.

Run `format-check` from a configured build. Format only source files, never
imported documentation, generated files, or build output.

## Tests

All checks are registered in `tests/CMakeLists.txt`.

| Path | Contents |
|---|---|
| `tests/unit/` | Component checks grouped like `src/` |
| `tests/integration/` | CLI, catalog corpus/import, embedded manual, idle behavior, and API consumer |
| `tests/contracts/` | Source dependency boundaries and formatting checks |
| `tests/fixtures/` | Self-authored HTML and formatting inputs |
| `tests/support/` | Shared document and frame helpers |
| `tests/tools/` | Optional Windows runtime observation script |

Run `ctest --preset linux-mlc-tests`. Corpus integration cases skip when the
explicitly downloaded sample library is absent. Test changes involving native
input, scrolling, resizing, or terminal restoration in a terminal as well.
Downloads and generated artifacts stay outside source control.

See [adapter development](adapters.md) for source-family registration and
[local libraries](local-documents.md) for the import/catalog contract.
