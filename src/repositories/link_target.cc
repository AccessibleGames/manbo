#include "repositories/link_target.h"

#include <libxml/uri.h>
#include <libxml/xmlmemory.h>

#include <algorithm>
#include <cctype>

namespace manbo {
namespace {
using Uri = std::unique_ptr<xmlURI, decltype(&xmlFreeURI)>;
using XmlText = std::unique_ptr<xmlChar, decltype(xmlFree)>;

std::string
Decode(const char* text) {
  if (!text)
    return {};
  std::unique_ptr<char, decltype(xmlFree)> decoded(
    xmlURIUnescapeString(text, 0, nullptr), xmlFree);
  return decoded ? std::string(decoded.get()) : std::string{};
}

bool
WebScheme(const char* scheme) {
  if (!scheme)
    return true;
  std::string name(scheme);
  std::ranges::transform(name, name.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return name == "http" || name == "https";
}
}  // namespace

Result<LinkTarget>
ResolveLink(const Document& origin, std::string_view reference) {
  const auto invalid = [](std::string message) {
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, std::move(message)});
  };
  if (reference.empty() || reference.size() > 4096 ||
      reference.find('\\') != reference.npos ||
      reference.find("%00") != reference.npos ||
      std::ranges::any_of(
        reference, [](unsigned char character) { return character < 0x20; }))
    return invalid("Invalid documentation reference");

  // Raw parsing retains percent escapes in offline archive filenames. Some
  // upstream books store operator%3D.html literally; others store operator=.
  const std::string text(reference);
  Uri uri(xmlParseURIRaw(text.c_str(), 1), xmlFreeURI);
  if (!uri || !WebScheme(uri->scheme) || uri->user)
    return invalid(
      "Only installed HTTP(S) or relative references are supported");

  LinkTarget target;
  target.anchor = Decode(uri->fragment);
  target.same_page = !uri->scheme && !uri->server && !uri->path && !uri->query;
  if (target.same_page)
    return target;

  if (!uri->scheme && !uri->server && uri->path && uri->path[0] != '/' &&
      !uri->query && !origin.source_file.empty()) {
    const auto parent = origin.source_file.parent_path();
    target.files.push_back(
      (parent / std::filesystem::u8path(uri->path)).lexically_normal());
    const auto decoded =
      (parent / std::filesystem::u8path(Decode(uri->path))).lexically_normal();
    if (target.files.front() != decoded)
      target.files.push_back(decoded);
  }

  XmlText absolute(
    xmlBuildURI(BAD_CAST text.c_str(), BAD_CAST origin.source_url.c_str()),
    xmlFree);
  if (absolute) {
    // Absolute links on the same upstream site can still identify a page in
    // this archive. libxml2 leaves a different authority as an absolute URI.
    XmlText relative(
      xmlBuildRelativeURI(absolute.get(), BAD_CAST origin.source_url.c_str()),
      xmlFree);
    Uri local(relative ? xmlParseURIRaw(
                           reinterpret_cast<const char*>(relative.get()), 1)
                       : nullptr,
      xmlFreeURI);
    if (local && !local->scheme && !local->server && local->path &&
        local->path[0] != '/' && !local->query && !origin.source_file.empty()) {
      const auto parent = origin.source_file.parent_path();
      for (const auto& path : {std::string(local->path), Decode(local->path)}) {
        const auto file =
          (parent / std::filesystem::u8path(path)).lexically_normal();
        if (std::ranges::find(target.files, file) == target.files.end())
          target.files.push_back(file);
        // The cppreference book adds .html to canonical /w/... page paths.
        if (origin.family == SourceFamily::kCppReference &&
            !path.ends_with(".html")) {
          auto html_file = file;
          html_file += ".html";
          target.files.push_back(std::move(html_file));
        }
      }
    }

    Uri resolved(
      xmlParseURI(reinterpret_cast<const char*>(absolute.get())), xmlFreeURI);
    if (resolved && resolved->scheme && WebScheme(resolved->scheme)) {
      xmlFree(resolved->fragment);
      resolved->fragment = nullptr;
      XmlText saved(xmlSaveUri(resolved.get()), xmlFree);
      if (saved)
        target.source_url = reinterpret_cast<const char*>(saved.get());
    }
  }
  return target;
}
}  // namespace manbo
