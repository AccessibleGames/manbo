#include "core/browser.h"

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <ranges>
#include <type_traits>

namespace manbo {
namespace {
std::string
ResultStatus(std::size_t count) {
  return fmt::format("{} result{}", count, count == 1 ? "" : "s");
}
}  // namespace

void
Browser::RememberPage() {
  if (!state_.document)
    return;
  // Keep back navigation bounded, and move source bytes instead of copying
  // them. Adapted content is already shared and immutable.
  constexpr std::size_t kHistoryLimit = 16;
  if (history_.size() == kHistoryLimit)
    history_.erase(history_.begin());
  history_.push_back({std::move(*state_.document),
    state_.scroll_line,
    std::move(state_.document_anchor)});
  state_.document.reset();
}

Transition
Browser::Dispatch(const Event& event) {
  if (!state_.running)
    return {};
  const auto request_selected_preview = [&]() -> Transition {
    state_.preview.reset();
    state_.preview_error.clear();
    if (state_.hits.empty()) {
      state_.preview_pending = false;
      return {true, false, std::nullopt};
    }
    state_.preview_pending = true;
    // A later selection increments the generation and invalidates this reply.
    return {true,
      false,
      ReadRequest{
        ++state_.generation, state_.hits.at(state_.selection).id, true}};
  };
  return std::visit(
    [&](const auto& event_action) -> Transition {
      using T = std::decay_t<decltype(event_action)>;
      if constexpr (std::is_same_v<T, ShowSearch>) {
        history_.clear();
        following_link_ = false;
        expected_page_.clear();
        direct_open_ = false;
        const auto generation = state_.generation + 1;
        state_ = {};
        state_.generation = generation;
        state_.search_home = true;
        state_.status = "Enter a symbol, command, or topic";
        return {true, true, std::nullopt};

      } else if constexpr (std::is_same_v<T, Search>) {
        if (state_.generation != 0 && state_.route == Route::kResults &&
            !state_.search_home && state_.query == event_action.query)
          return {};
        history_.clear();
        following_link_ = false;
        state_.document_anchor.clear();
        state_.search_home = false;
        expected_page_.clear();
        direct_open_ = false;
        state_.query = event_action.query;
        state_.route = Route::kResults;
        state_.document.reset();
        state_.preview.reset();
        state_.preview_pending = false;
        state_.preview_error.clear();
        state_.hits.clear();
        state_.selection = 0;
        state_.scroll_line = 0;
        state_.pending = true;
        state_.status = "Searching offline documents...";
        return {
          true, false, SearchRequest{++state_.generation, event_action.query}};

      } else if constexpr (std::is_same_v<T, Select>) {
        if (state_.route != Route::kResults || state_.pending ||
            state_.hits.empty())
          return {};
        const auto next_selection = std::clamp(
          static_cast<std::int64_t>(state_.selection) + event_action.delta,
          std::int64_t{0},
          static_cast<std::int64_t>(state_.hits.size() - 1));
        if (static_cast<std::size_t>(next_selection) == state_.selection)
          return {};
        state_.selection = static_cast<std::size_t>(next_selection);
        // The session renders this selection before starting the preview read.
        return request_selected_preview();

      } else if constexpr (std::is_same_v<T, OpenSelected>) {
        if (state_.route != Route::kResults || state_.pending ||
            state_.hits.empty())
          return {};
        history_.clear();
        following_link_ = false;
        state_.document_anchor.clear();
        const auto selected_page_id = state_.hits.at(state_.selection).id;
        expected_page_ = selected_page_id;
        direct_open_ = false;
        state_.route = Route::kReader;
        state_.scroll_line = 0;
        state_.preview_error.clear();
        if (state_.preview && state_.preview->id == selected_page_id) {
          ++state_.generation;
          state_.document = state_.preview;
          state_.pending = false;
          state_.preview_pending = false;
          state_.status = "Offline page";
          return {true, false, std::nullopt};
        }
        state_.document.reset();
        state_.preview_pending = false;
        state_.pending = true;
        state_.status = "Loading page...";
        return {
          true, false, ReadRequest{++state_.generation, selected_page_id}};

      } else if constexpr (std::is_same_v<T, OpenPage>) {
        if (event_action.reference.empty())
          return {};
        following_link_ = false;
        if (state_.route != Route::kReader) {
          history_.clear();
          state_.document_anchor.clear();
          state_.scroll_line = 0;
        }
        state_.search_home = false;
        expected_page_ = event_action.reference;
        direct_open_ = true;
        state_.route = Route::kReader;
        state_.preview_pending = false;
        state_.pending = true;
        state_.status = fmt::format("Opening {}...", event_action.reference);
        return {true,
          false,
          ReadRequest{++state_.generation, event_action.reference}};

      } else if constexpr (std::is_same_v<T, FollowLink>) {
        if (state_.route != Route::kReader || !state_.document ||
            state_.pending || event_action.target.empty())
          return {};
        following_link_ = true;
        state_.pending = true;
        state_.status = "Opening offline reference...";
        return {true,
          false,
          LinkRequest{
            ++state_.generation, state_.document->id, event_action.target}};

      } else if constexpr (std::is_same_v<T, Back>) {
        if (state_.route == Route::kResults)
          return {};
        ++state_.generation;
        following_link_ = false;
        if (state_.pending && state_.document) {
          state_.pending = false;
          state_.status = "Offline page";
          return {true, true, std::nullopt};
        }
        if (!history_.empty()) {
          auto previous = std::move(history_.back());
          history_.pop_back();
          state_.document = std::move(previous.document);
          state_.scroll_line = previous.scroll_line;
          state_.document_anchor = std::move(previous.anchor);
          state_.pending = false;
          state_.status = "Offline page";
          return {true, true, std::nullopt};
        }
        state_.route = Route::kResults;
        state_.document.reset();
        state_.document_anchor.clear();
        state_.scroll_line = 0;
        state_.pending = false;
        state_.search_home = state_.hits.empty() && state_.query.empty();
        state_.status = ResultStatus(state_.hits.size());
        if (!state_.hits.empty() &&
            (!state_.preview ||
              state_.preview->id != state_.hits.at(state_.selection).id)) {
          auto preview_transition = request_selected_preview();
          preview_transition.cancel_work = true;
          return preview_transition;
        }
        return {true, true, std::nullopt};

      } else if constexpr (std::is_same_v<T, Scroll>) {
        if (state_.route != Route::kReader || !state_.document)
          return {};
        const auto fallback = static_cast<std::size_t>(
          std::ranges::count(state_.document->body, '\n'));
        const auto max_scroll_line =
          std::min(event_action.max_offset.value_or(fallback),
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
        const auto current_scroll_line =
          std::min(event_action.from_offset.value_or(state_.scroll_line),
            max_scroll_line);
        const auto next = std::clamp(
          static_cast<std::int64_t>(current_scroll_line) + event_action.lines,
          std::int64_t{0},
          static_cast<std::int64_t>(max_scroll_line));
        if (static_cast<std::size_t>(next) == state_.scroll_line &&
            state_.document_anchor.empty())
          return {};
        state_.scroll_line = static_cast<std::size_t>(next);
        state_.document_anchor.clear();
        return {true, false, std::nullopt};

      } else if constexpr (std::is_same_v<T, ScrollTo>) {
        if (state_.route != Route::kReader || !state_.document)
          return {};
        state_.scroll_line =
          std::min(event_action.line, event_action.max_offset);
        state_.document_anchor.clear();
        // A find can select a different occurrence on the same rendered line.
        return {true, false, std::nullopt};

      } else if constexpr (std::is_same_v<T, Refresh>) {
        return {true, false, std::nullopt};

      } else if constexpr (std::is_same_v<T, Quit>) {
        state_.running = false;
        state_.pending = false;
        state_.preview_pending = false;
        ++state_.generation;
        return {false, true, std::nullopt};

      } else if constexpr (std::is_same_v<T, SearchCompleted>) {
        if (event_action.id != state_.generation || !state_.pending ||
            state_.route != Route::kResults)
          return {};
        state_.hits = event_action.hits;
        state_.selection = 0;
        state_.pending = false;
        state_.status = ResultStatus(state_.hits.size());
        return request_selected_preview();

      } else if constexpr (std::is_same_v<T, ReadCompleted>) {
        if (event_action.id != state_.generation)
          return {};
        if (event_action.preview) {
          if (state_.route != Route::kResults || !state_.preview_pending ||
              state_.hits.empty())
            return {};
          if (event_action.document.id != state_.hits.at(state_.selection).id) {
            state_.preview_pending = false;
            state_.preview_error = "Repository returned a different page";
            return {true, false, std::nullopt};
          }
          state_.preview = event_action.document;
          state_.preview_pending = false;
          return {true, false, std::nullopt};
        }
        if (!state_.pending || state_.route != Route::kReader ||
            following_link_)
          return {};
        const auto& document = event_action.document;
        const auto document_label =
          document.manual_section.empty()
            ? document.title
            : fmt::format("{}({})", document.title, document.manual_section);
        const bool matches_expected_page =
          direct_open_ ? expected_page_ == document.id ||
                           expected_page_ == document.title ||
                           expected_page_ == document_label
                       : expected_page_ == document.id;
        if (!matches_expected_page) {
          state_.pending = false;
          state_.status = "Error: Repository returned a different page";
          return {true, false, std::nullopt};
        }
        RememberPage();
        state_.document_anchor.clear();
        state_.document = event_action.document;
        if (!state_.hits.empty() &&
            state_.hits.at(state_.selection).id == event_action.document.id)
          state_.preview = event_action.document;
        state_.scroll_line = 0;
        state_.pending = false;
        state_.status = "Offline page";
        return {true, false, std::nullopt};

      } else if constexpr (std::is_same_v<T, LinkCompleted>) {
        if (event_action.id != state_.generation || !state_.pending ||
            state_.route != Route::kReader || !following_link_)
          return {};
        RememberPage();
        state_.document = event_action.page.document;
        state_.document_anchor = event_action.page.anchor;
        state_.scroll_line = 0;
        state_.pending = false;
        following_link_ = false;
        state_.status = "Offline page";
        return {true, false, std::nullopt};

      } else if constexpr (std::is_same_v<T, JobFailed>) {
        if (event_action.id != state_.generation)
          return {};
        if (state_.preview_pending && state_.route == Route::kResults) {
          state_.preview_pending = false;
          state_.preview_error = event_action.error.message;
          return {true, false, std::nullopt};
        }
        if (!state_.pending)
          return {};
        state_.pending = false;
        state_.status = fmt::format("Error: {}", event_action.error.message);
        return {true, false, std::nullopt};
      }
      return {};
    },
    event);
}

}  // namespace manbo
