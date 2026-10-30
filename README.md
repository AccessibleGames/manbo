# Manbo

A C++ terminal browser for local HTML collections, cppreference, POSIX, GNU
manuals, and formatted man pages. Search a library, preview a result, follow
references, and return to the same reader position.

## Build

Build on Linux in Conda `mlc`. The application and its library utilities do not
require Python or a network connection.

```sh
conda activate mlc
cmake --preset linux-mlc
cmake --build --preset linux-mlc
./build/linux/debug/manbo
```

Dependencies are listed in `environment-linux.yml`. Install missing packages
when provisioning the environment. Windows presets and build conventions are
in [development notes](docs/development.md).

## Local documents

Run these commands from this repository's `manbo/` directory. Replace the source
path with an extracted HTML collection:

```sh
./build/linux/debug/manbo import html /path/to/guide \
  --pack project-guide --edition 1.0 --data data/library
./build/linux/debug/manbo
```

`manbo` defaults to the TUI; `manbo tui` is the explicit equivalent. It opens an
editable search prompt immediately. Enter submits a query; an empty query lists
installed pages. Pipes and scripts use explicit `search` or `read` commands.

```sh
./build/linux/debug/manbo search socket --json
./build/linux/debug/manbo read 'socket(3p)'
./build/linux/debug/manbo index --data data/library
./build/linux/debug/manbo format /path/to/page.html --width 80
./build/linux/debug/manbo manbo
```

Imports retain source hierarchy, assets, notices, and SHA256 hashes. Each pack
has a stable name and its own source folder. Changed editions use new pack names;
reimporting identical files is safe. The C++ importer prepares the SQLite
metadata index. Browsing performs no downloads or source rewrites.

See [local library management](docs/local-documents.md) for cppreference/POSIX
imports, formatted manuals, catalog fields, and edition management. See
[adapter development](docs/adapters.md) when a collection needs different HTML
interpretation.

In the TUI, Enter opens a result, j/k select or scroll, `/` searches or finds,
and `:` enters a documentation command. Tab/Shift+Tab select references, Enter
follows them, Esc returns to the previous page or results, and q quits. The
built-in `manbo(1)` manual works without an installed library.

## Checks

```sh
cmake --preset linux-mlc-tests
cmake --build --preset linux-mlc-tests
ctest --preset linux-mlc-tests
cmake --build --preset linux-mlc-tests --target format-check
```

Linux outputs live in `build/linux/`; Windows outputs live in `build/windows/`.

## Repository

| Directory | Contents |
|---|---|
| `app/manbo/` | CLI commands and application composition |
| `src/` | Browser, library import, repositories, adapters, runtime, and UI |
| `tests/` | Unit/integration checks, contracts, fixtures, and support |
| `docs/` | Command manual, local library and adapter guides, development notes |

See [architecture](docs/architecture.md) to follow the code.

## Current limits

Inputs are extracted HTML folders or UTF-8 text. Raw roff/mdoc needs a formatter
such as man-db/groff before text import. Native archive extraction and downloads
are deferred. Search covers metadata; indexed body search remains unfinished.
Images use alternative text, and CSS-dependent diagrams need further work.
