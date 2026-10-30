# Architecture

The application uses C++20 interfaces and Boost.Asio coroutines, channels, and
thread pools.

```text
console input or worker reply
  -> Browser::Dispatch
  -> Session renders and flushes the new state
  -> optional repository request runs in a worker
  -> immutable reply returns through the event channel
```

`Browser` owns navigation state. `Session` owns the UI event flow. Workers never
write terminal output or resume UI coroutines. Input and worker completion use
blocking waits; the application has no periodic redraw or idle timer. The
native console feeds an Asio coroutine on the UI owner thread. Asio dispatch
runs after actual input or completion events.

`EventChannel` only bridges Boost's channel to the synchronous console, native
wake signal, and repository stop tokens. Boost owns queueing, backpressure,
coroutine frames, and worker threads. The synchronous channel adapter uses one
sleeping Asio completion thread, separate from blocking document workers.
Manbo owns the latest-request policy and document-specific error handling.

A launch without a query renders the search editor without a repository request.
Imports prepare a SQLite metadata index; searches read matching summaries and
page reads resolve and validate one source path. A missing or stale index uses
the JSON catalog. Browsing never rebuilds indexes or modifies the source library.

Reference reads use libxml2 URI resolution and exact catalog file/URL lookups.
Unlisted assets and external URLs are never opened. The browser keeps a bounded
stack of immutable pages and reader positions; back navigation needs no job.
Adapters preserve named anchors, including table rows, independently of terminal
width. The renderer maps those anchors and link styles to the current layout.

## Ownership

| Path | Responsibility | Start reading at |
|---|---|---|
| `app/manbo/` | Parse CLI input and compose repositories, jobs, and UI | `main.cc`, `repository_setup.cc` |
| `src/core/` | Events, requests, state transitions, and command grammar | `interfaces.h`, `browser.cc`, `command.cc` |
| `src/runtime/` | Asio adapters, latest-request policy, and session loop | `channel.cc`, `jobs.cc`, `session.cc` |
| `src/platform/` | Native console and opaque wake signals | `console.h`, `linux.cc`, `windows.cc` |
| `src/documents/` | Immutable semantic document types and adapter dispatch | `content.h`, `content.cc` |
| `src/documents/adapters/` | HTML parser and family-specific interpretation | `html/adapter.cc`, each family's `adapter.cc` |
| `src/library/` | Import local trees, preserve sources/manifests, and rebuild indexes | `library.h`, `library.cc` |
| `src/repositories/` | Read indexed metadata, source pages, samples, and the manual | `catalog_index.cc`, `offline_repository.cc` |
| `src/ui/terminal/` | Input modes, document layout, and frame output | `terminal.cc`, `document_view.cc`, `table_layout.cc` |

Native OS headers stay inside the platform implementations. Core, runtime,
documents, and repositories have no UI dependency. Headers live beside their
implementations; includes use paths relative to `src/`, such as
`core/browser.h`.

## Documents and rendering

The repository retains original bytes in `Document::body`. Adapters produce
shared immutable `RichDocument` blocks; the UI lays them out for the viewport.

- HTML parsing is bounded by size, depth, node, and table limits. It does not
  execute scripts, fetch assets, or resolve external entities.
- The shared walker groups inline text into paragraphs and preserves headings,
  definitions, lists, tables, code, and preformatted text.
- Family adapters choose document roots, omit website navigation, and interpret
  layout conventions such as cppreference declaration/version tables.
- Terminal layout wraps styled words together, sizes table columns by content,
  and stacks tables when columns would be too narrow.
- Renderers consume semantic content; they never modify source files.

## Local library maintenance

The C++ CLI owns explicit `import html`, `import text`, `index`, and `format`
commands. `src/library/` copies and validates local collections; repository
readers remain read-only. SHA256 comes from OpenSSL, HTML interpretation from
libxml2, and index publication from SQLite plus filesystem staging. Importing
keeps packs separate and preserves their original hierarchy, assets, and notices.

The application defaults to the TUI. Redirected users select `search`, `read`,
or `format` explicitly. No production command invokes Python. Extract archives
before import; raw roff formatting remains an external tool's responsibility.

See [library management](local-documents.md) and [adapter development](adapters.md).

## Reusing the libraries

Include `manbo.h` and link `manbo::api` for the non-UI API. A terminal frontend
also includes `ui/terminal/terminal.h` and links `manbo_terminal_ui`.
`linux-mlc-library-only` builds the reusable libraries and a consumer check
without the application, terminal UI, CLI11, or GTest.
