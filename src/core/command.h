#ifndef MANBO_COMMAND_H_
#define MANBO_COMMAND_H_

#include <string_view>

#include "core/types.h"

namespace manbo {
// Small documentation-command grammar, not shell parsing or process execution.
// man name lists candidates; man section name opens an exact manual section.
auto
ParseCommand(std::string_view text) -> Result<Event>;
auto
ParseStartupQuery(std::string_view text) -> Result<Event>;
}  // namespace manbo

#endif  // MANBO_COMMAND_H_
