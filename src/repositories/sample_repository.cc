#include "repositories/sample_repository.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <ranges>
#include <utility>

namespace manbo {
namespace {
std::string
Lowercase(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  std::ranges::transform(text, std::back_inserter(result), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

std::string
Compact(std::string_view text) {
  std::string result;
  for (char c : text | std::views::filter([](unsigned char value) {
         return std::isalnum(value) != 0;
       }))
    result += c;
  return result;
}

bool
IsSubsequence(std::string_view needle, std::string_view haystack) {
  std::size_t n = 0;
  for (const auto c : haystack)
    if (n < needle.size() && needle[n] == c)
      ++n;
  return n == needle.size();
}

SearchHit
MakeSearchHit(const Document& doc) {
  return {doc.id, doc.title, doc.manual_section, doc.description, doc.source};
}

Result<Document>
PrepareDocument(Document document, std::stop_token stop) {
  const auto language = document.id.starts_with("cpp/")  ? "cpp"
                        : document.manual_section == "1" ? "sh"
                                                         : "c";
  auto content = AdaptContent({document.body, document.format, language}, stop);
  if (!content)
    return tl::make_unexpected(content.error());
  document.content = std::move(*content);
  return document;
}
}  // namespace

SampleRepository::SampleRepository()
    : documents_{
        {"cpp/forward-list",
          "std::forward_list",
          "",
          "A singly linked C++ container with insertion and erasure after an "
          "iterator.",
          "NAME\n  std::forward_list - singly linked sequence container\n\n"
          "SYNOPSIS\n  #include <forward_list>\n  std::forward_list<int> "
          "values{1, 2, 3};\n\n"
          "DESCRIPTION\n  These are self-authored prototype notes, not a "
          "complete reference.\n"
          "  Traversal is forward-only. There is no random-access indexing.\n"
          "  Insertion and erasure use the position before the affected "
          "elements.\n\n"
          "EXAMPLES\n  values.insert_after(values.before_begin(), 0);\n\n"
          "SEE ALSO\n  std::forward_list::insert_after, std::vector\n",
          "Manbo C++ samples"},
        {"cpp/insert-after",
          "std::forward_list::insert_after",
          "",
          "Insert elements after an iterator in a singly linked forward list.",
          "NAME\n  std::forward_list::insert_after\n\n"
          "SYNOPSIS\n  auto pos = values.before_begin();\n  "
          "values.insert_after(pos, 42);\n\n"
          "DESCRIPTION\n  A valid position identifies the element before the "
          "insertion.\n"
          "  The before-begin iterator permits insertion at the front.\n\n"
          "NOTE\n  This fixture omits overloads, requirements, and exception "
          "details.\n",
          "Manbo C++ samples"},
        {"cpp/vector",
          "std::vector",
          "",
          "A contiguous dynamic C++ array with random access.",
          "NAME\n  std::vector - contiguous dynamic sequence\n\n"
          "SYNOPSIS\n  #include <vector>\n  std::vector<int> values{1, 2, "
          "3};\n\n"
          "DESCRIPTION\n  Elements can be accessed by an index.\n"
          "  Reallocation invalidates pointers, references, and iterators to "
          "elements.\n\n"
          "NOTE\n  Consult a real versioned reference for all invalidation "
          "rules.\n",
          "Manbo C++ samples"},
        {"man/printf/1",
          "printf",
          "1",
          "Format arguments as shell-command output.",
          "NAME\n  printf(1) - format command arguments\n\n"
          "SYNOPSIS\n  printf FORMAT [ARGUMENT...]\n\n"
          "EXAMPLES\n  printf '%s\\n' hello\n\n"
          "NOTE\n  This is a self-authored sample, not a system-installed "
          "manual.\n"
          "  printf(1) and printf(3) describe different interfaces.\n",
          "Manbo Unix samples"},
        {"man/printf/3",
          "printf",
          "3",
          "Formatted C library output to a stream.",
          "NAME\n  printf(3) - formatted C library output\n\n"
          "SYNOPSIS\n  #include <stdio.h>\n  int printf(const char *format, "
          "...);\n\n"
          "DESCRIPTION\n  Writes formatted output to standard output.\n"
          "  Match each format conversion to the corresponding argument "
          "type.\n\n"
          "SEE ALSO\n  printf(1), fprintf(3), snprintf(3)\n",
          "Manbo Unix samples"},
        {"man/socket/2",
          "socket",
          "2",
          "Create a socket endpoint for communication.",
          "NAME\n  socket(2) - create a communication endpoint\n\n"
          "SYNOPSIS\n  #include <sys/socket.h>\n  int socket(int domain, int "
          "type, int protocol);\n\n"
          "DESCRIPTION\n  This fixture demonstrates a manual-section label "
          "and readable synopsis.\n"
          "  It does not implement sockets, Linux compatibility, or an actual "
          "manual importer.\n",
          "Manbo Unix samples"}} {
}

Result<std::vector<SearchHit>>
SampleRepository::Search(std::string_view query, std::stop_token stop) {
  const auto q = Lowercase(query);
  struct ScoredHit {
    int score;
    SearchHit hit;
  };
  std::vector<ScoredHit> scored;
  for (const auto& doc : documents_) {
    if (stop.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "Search cancelled"});
    const auto title = Lowercase(doc.title);
    const auto label = doc.manual_section.empty()
                         ? title
                         : fmt::format("{}({})", title, doc.manual_section);
    const auto content =
      Lowercase(fmt::format("{} {} {}", doc.title, doc.description, doc.body));
    int score = q.empty() ? 1 : 0;
    if (!q.empty() && (q == title || q == label || q == Lowercase(doc.id)))
      score = 1000;
    else if (!q.empty() && title.starts_with(q))
      score = 750;
    else if (!q.empty() && title.find(q) != std::string::npos)
      score = 600;
    else if (!q.empty()) {
      bool all = true;
      std::size_t start = 0;
      while (start < q.size()) {
        start = q.find_first_not_of(' ', start);
        if (start == std::string::npos)
          break;
        const auto end = q.find(' ', start);
        if (content.find(q.substr(start, end - start)) == std::string::npos) {
          all = false;
          break;
        }
        start = end == std::string::npos ? q.size() : end + 1;
      }
      if (all)
        score = 400;
      else {
        const auto needle = Compact(q);
        if (needle.size() >= 3 && IsSubsequence(needle, Compact(title)))
          score = 100;
      }
    }
    if (score > 0)
      scored.push_back({score, MakeSearchHit(doc)});
  }
  std::ranges::stable_sort(scored, std::greater{}, &ScoredHit::score);
  std::vector<SearchHit> result;
  result.reserve(scored.size());
  for (auto& entry : scored)
    result.push_back(std::move(entry.hit));
  return result;
}

Result<Document>
SampleRepository::Read(std::string_view page_id, std::stop_token stop) {
  std::optional<Document> result;
  for (const auto& doc : documents_) {
    if (stop.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "Read cancelled"});
    const auto label = doc.manual_section.empty()
                         ? doc.title
                         : fmt::format("{}({})", doc.title, doc.manual_section);
    if (doc.id == page_id)
      return PrepareDocument(doc, stop);
    if (doc.title == page_id || label == page_id) {
      if (result)
        return tl::make_unexpected(Error{ErrorCode::kAmbiguous,
          "Ambiguous page name; specify a section label or page ID"});
      result = doc;
    }
  }
  if (!result)
    return tl::make_unexpected(
      Error{ErrorCode::kNotFound, "No such sample page"});
  return PrepareDocument(std::move(*result), stop);
}

}  // namespace manbo
