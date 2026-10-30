#include <fmt/format.h>

#include "ui/terminal/view.h"
#include "view_internal.h"

namespace manbo {
namespace {
struct Ink {
  bool bold = false;
  bool dim = false;
  bool italic = false;
  bool underline = false;
  bool reverse = false;
};
auto
Resolve(const StyledSpan& span) -> Ink {
  Ink ink;
  ink.bold = span.bold;
  ink.italic = span.italic;
  ink.underline = span.underline;
  switch (span.role) {
    case StyleRole::kBrand:
    case StyleRole::kHeading:
    case StyleRole::kStrong:
    case StyleRole::kKeyword:
    case StyleRole::kTableHeader:
    case StyleRole::kError:
      ink.bold = true;
      break;
    case StyleRole::kMuted:
    case StyleRole::kSelectedMeta:
    case StyleRole::kBorder:
      ink.dim = true;
      break;
    case StyleRole::kComment:
      ink.dim = true;
      ink.italic = true;
      break;
    case StyleRole::kLink:
      ink.underline = true;
      break;
    case StyleRole::kSelected:
    case StyleRole::kMatch:
      ink.reverse = true;
      break;
    case StyleRole::kCurrentMatch:
      ink.reverse = true;
      ink.bold = true;
      break;
    default:
      break;
  }
  return ink;
}

auto
Sgr(const StyledSpan& span) -> std::string {
  const auto ink = Resolve(span);
  // 39/49 inherit the terminal's configured default colors. No RGB/palette
  // fill.
  return fmt::format("\x1b[0;39;49{}{}{}{}{}m",
    ink.bold ? ";1" : "",
    ink.dim ? ";2" : "",
    ink.italic ? ";3" : "",
    ink.underline ? ";4" : "",
    ink.reverse ? ";7" : "");
}

auto
Escape(std::string_view text) -> std::string {
  std::string result;
  for (char c : text) {
    switch (c) {
      case '&':
        result += "&amp;";
        break;
      case '<':
        result += "&lt;";
        break;
      case '>':
        result += "&gt;";
        break;
      case '"':
        result += "&quot;";
        break;
      case '\'':
        result += "&#39;";
        break;
      default:
        result += c;
    }
  }
  return result;
}
}  // namespace

auto
SerializeAnsi(const StyledFrame& frame) -> std::string {
  std::string output = "\x1b[?25l";
  for (std::size_t row = 0; row < frame.lines.size(); ++row) {
    output += fmt::format("\x1b[{};1H", row + 1);
    const auto line = render_internal::Fit(
      frame.lines[row], frame.width - (row + 1 == frame.lines.size() ? 1 : 0));
    for (const auto& span : line)
      output += Sgr(span) + SafeText(span.text);
    output += Sgr({""}) + "\x1b[K";
  }
  if (frame.cursor)
    output += fmt::format(
      "\x1b[{};{}H\x1b[?25h", frame.cursor->row + 1, frame.cursor->column + 1);
  return output;
}

auto
SerializeHtml(const StyledFrame& frame) -> std::string {
  std::string output =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src "
    "'none'; style-src 'unsafe-inline'\">"
    "<title>Manbo — rendered terminal frame</title><style>"
    "html{color-scheme:light "
    "dark}body{margin:0;background:Canvas;color:CanvasText}"
    "main{display:inline-block;padding:16px;font:14px/22px 'Cascadia "
    "Mono',Consolas,monospace}"
    ".row{white-space:pre;height:22px}span{white-space:pre;display:inline-"
    "block;height:22px;line-height:22px;vertical-align:top}"
    "</style></head><body><main aria-label=\"Terminal frame exported by the "
    "C++ renderer\">";
  for (const auto& line : frame.lines) {
    output += "<div class=\"row\">";
    for (const auto& span : line) {
      const auto ink = Resolve(span);
      output += fmt::format(
        "<span "
        "style=\"color:{};background:{};font-weight:{};font-style:{};text-"
        "decoration:{};opacity:{}\">{}</span>",
        ink.reverse ? "Canvas" : "inherit",
        ink.reverse ? "CanvasText" : "transparent",
        ink.bold ? "700" : "400",
        ink.italic ? "italic" : "normal",
        ink.underline ? "underline" : "none",
        ink.dim ? "0.75" : "1",
        Escape(SafeText(span.text)));
    }
    output += "</div>";
  }
  output += "</main></body></html>";
  return output;
}
}  // namespace manbo
