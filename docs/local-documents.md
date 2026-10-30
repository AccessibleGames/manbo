# Local document libraries

Commands below assume a build in `build/linux/debug/`. Use Conda `mlc` when
building. Import, index, and format are C++ commands; they need no Python or
network access. Close a TUI using the library before changing its catalog.

## Layout

A library is a movable directory containing a catalog and its sources:

```text
data/library/
  catalog.json                 # Page identities, metadata, and relative paths
  catalog.sqlite3              # Derived search index
  manifests/
    project-guide.json         # Source location, edition, file hashes, import time
  sources/
    project-guide/              # Unchanged exported tree, including assets/notices
      index.html
      api/open.html
      style.css
      NOTICE
```

Manbo discovers `data/library/` by searching the working directory and executable
ancestors. It falls back to `data/official-samples/` when present. Use
`--data /path/to/library` to select another library explicitly. Libraries, indexes,
and exported review frames stay out of Git.

## Import an HTML folder

Keep the exported directory structure intact, including encoded filenames and
support files. The default `generic` adapter handles ordinary HTML headings,
paragraphs, lists, definitions, tables, and code:

```sh
./build/linux/debug/manbo import html /path/to/exported-guide \
  --pack project-guide --edition 1.0 --data data/library
./build/linux/debug/manbo --data data/library
./build/linux/debug/manbo search guide --data data/library --json
```

Each `.html`/`.htm` page gets a title from `<title>` (or its filename), a description
from adapted prose, and an ID such as `project-guide/api/open`. Other regular
files are preserved as support files. A single HTML file can be imported too;
import a whole tree when its pages need relative references or assets.

Document bytes must be valid UTF-8. Imports reject pages with no visible adapted
content; use complete HTML exports with an explicit body rather than head-only
fragments. Original files are never transcoded to make an import succeed.

`--source-url https://example.org/docs/` optionally records the upstream base URL
for the imported root. It helps match absolute references to installed pages and
never causes a fetch. Without it, source URLs are empty and relative local
references still work. Pack names use lowercase letters, digits, and hyphens.

## cppreference and POSIX

Start with already extracted folders. `--pages` selects the subtree to index
while the entire input tree, including shared assets and notices, is preserved.
IDs and upstream URLs are relative to that subtree:

```sh
./build/linux/debug/manbo import html /path/to/cppreference-book --pages reference/en \
  --family cppreference --pack cppref-2025-02-09 --edition 2025-02-09 \
  --source-url https://en.cppreference.com/w/ --data data/library

./build/linux/debug/manbo import html /path/to/posix-export --pages susv5-html \
  --family posix --pack posix-2024 --edition 'Issue 8 (2024)' \
  --source-url https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/ \
  --data data/library
```

If the selected input already is `reference/en` or `susv5-html`, omit `--pages`.
Prefer the whole export tree when assets or notices live outside the page subtree.

The POSIX importer indexes document chapters and manuals, excluding contents/TOC
pages. All support files remain in the copied tree. GNU Texinfo exports can use
`--family gnu`; man7-style saved HTML can use `--family linux-man`. A family adapter
expects that family's markup. Try `generic` for unrelated HTML; see
[adapter development](adapters.md) if the resulting structure is wrong.

## Formatted text and manuals

Import already formatted UTF-8 text with:

```sh
./build/linux/debug/manbo import text /path/to/text-manuals \
  --family linux-man --pack system-manuals --edition installed --data data/library
```

Manual filenames must include their section, such as `socket.2.txt` or
`pthread_create.3.txt`. This preserves `name(section)` selection and SEE ALSO
references. Use the default `generic` family for other `.txt` collections.

Raw roff/mdoc is a different format. On Linux, man-db/groff can produce the text
before import. For example, create a separate export directory and format one
source without changing it:

```sh
mkdir -p /tmp/manbo-man-export
MANWIDTH=88 MANPAGER=cat PAGER=cat LC_ALL=C.UTF-8 GROFF_NO_SGR=1 \
  env -u MAN_KEEP_FORMATTING -u MANOPT -u MANROFFOPT \
  man --nh --nj -l /usr/share/man/man2/socket.2.gz \
  > /tmp/manbo-man-export/socket.2.txt
```

Keep compressed originals and their copyright notices beside the `.txt` files
in that export tree if you want them copied into the library as support files.
Formatting raw sources needs Linux man-db/groff; the resulting library is portable.

## Editions, updates, and removal

A pack name identifies an immutable source tree. Reimport identical sources with
the same name to refresh metadata. If any source or support file changes, use a
new name, such as `project-guide-v2`. Both editions can share one catalog; IDs
include the pack name and exact IDs disambiguate repeated page titles.

Import validates pages and prepares files in a staging directory before publishing
catalog changes. Other packs remain registered. It refuses source/destination
nesting, symlinks, and modified existing pack files. Limits are 100,000 files,
4 GiB per tree, 64 MiB per support file, and 2 MiB per document; HTML structure
also has adapter limits. If indexing fails after import, the error says the
catalog was saved: fix the reported issue and run `manbo index` again.

To unregister pages, edit `catalog.json` and remove their records, then rebuild
the index. Keep the source folders until you have verified the remaining catalog.
Do not rename a pack's source files without updating their catalog paths.

## Catalog and index

Imported catalogs remain readable JSON. An entry needs these fields:

```json
{
  "schema_version": 1,
  "documents": [
    {
      "id": "project-guide/index",
      "title": "Project guide",
      "family": "generic",
      "format": "html",
      "file": "sources/project-guide/index.html",
      "source_url": ""
    }
  ]
}
```

`file` is relative to the library root, uses `/`, and must stay inside it. IDs
are unique and stable; titles need not be unique. Optional fields include
`description`, `manual_section`, `edition`, `pack`, `sha256`, and `bytes`.
`family` selects the adapter: `generic`, `cppreference`, `posix`, `gnu`, or
`linux-man`. `format` is `html`, or `plain` for generic/Linux-man text.

Imports build the SQLite index automatically. Rebuild after manual catalog edits
or copying a library between systems:

```sh
./build/linux/debug/manbo index --data data/library
```

A missing or stale index uses the JSON fallback. Reindexing also repairs a corrupt
index. Search indexes metadata rather than page bodies. Only cataloged pages can
be opened as references; copied support HTML is not implicitly searchable.

## Review formatting

`format` renders the whole document as plain terminal text. It does not rewrite
HTML, and `--output` must name a new file:

```sh
./build/linux/debug/manbo format /path/to/page.html --family generic --width 80
./build/linux/debug/manbo format /path/to/page.html --width 40 \
  --output build/review/page-40.txt
```

`snapshot` exports the actual browser frame for visual review:

```sh
./build/linux/debug/manbo snapshot --reader --source-html /path/to/page.html \
  --source-family generic --width 80 --height 24 --output build/review/page.html
```

Check varied pages at 40, 80, and 132 columns, including declarations, tables,
examples, and long references. Successful parsing alone does not establish good
layout. Use Tab/Shift+Tab, Enter, and Esc in the TUI to check local links and Back.
