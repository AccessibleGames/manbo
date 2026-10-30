# Adding a document adapter

Import and rendering have separate jobs. The importer preserves files and records
metadata; an `IContentAdapter` interprets one immutable source as `RichDocument`
blocks; terminal layout decides how those blocks fit the viewport.

Start with `--family generic`. Add an adapter when a source needs specific root
selection, navigation removal, or interpretation of layout tables and code.
Extend an existing family adapter for another edition of that same source.

## Source rules

Adapters live in `src/documents/adapters/<family>/`, each with its own header,
implementation, and CMake target. The
[POSIX adapter](../src/documents/adapters/posix/adapter.cc) is a compact example;
[cppreference](../src/documents/adapters/cppreference/adapter.cc) shows declaration
layout rules. Reuse the bounded libxml2 walker through
[HtmlRules](../src/documents/adapters/html/adapter_internal.h).

For a hypothetical `guide` family, declare `MakeGuideAdapter()` in
`src/documents/adapters/guide/adapter.h`, returning
`std::unique_ptr<IContentAdapter>` in namespace `manbo::documents_internal`.
Its implementation can begin with:

```cpp
#include "documents/adapters/guide/adapter.h"

#include "documents/adapters/html/adapter_internal.h"

namespace manbo::documents_internal {
namespace {
bool
Root(xmlNode* node) {
  return Name(node) == "main" && HasClass(node, "guide-content");
}

bool
Skip(xmlNode* node) {
  return HasClass(node, "guide-sidebar");
}

class GuideAdapter final : public IContentAdapter {
 public:
  Result<RichDocument>
  Adapt(SourceContent source, std::stop_token stop) const override {
    HtmlRules rules;
    rules.name = "Project guide";
    rules.root = Root;
    rules.skip = Skip;
    rules.language = "cpp";
    return ParseHtml(source, rules, stop);
  }
};
}  // namespace

std::unique_ptr<IContentAdapter>
MakeGuideAdapter() {
  return std::make_unique<GuideAdapter>();
}
}  // namespace manbo::documents_internal
```

Choose the root from actual source markup. The walker renders its children;
keep the whole document body, not one heading or the first paragraph. A required
root that is absent produces an error rather than an unrelated partial page.

| Rule | Purpose |
|---|---|
| `root` | Select the document content container |
| `marker` | Optionally verify source identity within the selected root |
| `skip` | Omit source-specific navigation and controls |
| `treatment` | Mark containers, code, preformatted blocks, or table headers |
| `language` | Label code blocks for rendering |
| `break_after` | Preserve source-specific explicit line boundaries |
| `separate_inline` | Restore spacing supplied by CSS, such as version badges |

Use `HasClass` for class tokens. Treat layout tables as containers only when they
are actually page layout; retain data tables, captions, row/column spans, and
headers. Do not flatten nested lists, split inline styles into paragraphs, or
turn all preformatted text into C++ code. Preserve link targets and named anchors
through the shared walker. Keep CSS and terminal escape sequences out of semantic
content, and never modify originals to make adaptation easier.

## Register the family

Add a CMake target in the new adapter directory:

```cmake
add_library(manbo_adapter_guide adapter.cc)
target_include_directories(manbo_adapter_guide PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(manbo_adapter_guide
  PUBLIC tl::expected
  PRIVATE manbo_adapter_html)
manbo_warnings(manbo_adapter_guide)
```

Then update these registration points:

| File | Change |
|---|---|
| `src/documents/content.h` | Add `SourceFamily::kGuide` |
| `src/documents/content.cc` | Include the header; update `SourceFamilyName`, `ParseSourceFamily`, and `MakeContentAdapter` |
| `src/documents/adapters/CMakeLists.txt` | Add `add_subdirectory(guide)` |
| `src/CMakeLists.txt` | Link `manbo_adapter_guide` privately to `manbo_content` |
| `app/manbo/main.cc` | Add `guide` to snapshot's `--source-family` accepted values |

The C++ importer/indexer uses `ParseSourceFamily`; it has no separate Python
registry. Generic folder discovery already covers ordinary HTML. If a source
needs page filtering or manual-section metadata, add that policy to
`src/library/library.cc`; keep rendering rules in the adapter.

## Validate

Put small, varied fixtures under `tests/fixtures/html/` and component checks in
`tests/unit/documents/content_test.cc`. Cover root selection, code, prose, tables,
lists, named anchors and links, malformed or unsupported input, cancellation,
limits, and unchanged source bytes. Avoid relying on one representative layout.

Build in `mlc`:

```sh
conda activate mlc
cmake --preset linux-mlc-tests
cmake --build --preset linux-mlc-tests
ctest --preset linux-mlc-tests
cmake --build --preset linux-mlc-tests --target format-check
```

After registration, use `manbo format page.html --family guide` and
`manbo snapshot --reader --source-html page.html --source-family guide
--output build/review/guide.html`. Review at narrow and wide widths, then import
a small collection and exercise links, scrolling, resizing, and Back in the TUI.
See [local library management](local-documents.md) for complete commands.
