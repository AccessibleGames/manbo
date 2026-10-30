#ifndef MANBO_RESULT_H_
#define MANBO_RESULT_H_

#include <string>

#include "tl/expected.hpp"

namespace manbo {

enum class ErrorCode {
  kCancelled,
  kNotFound,
  kAmbiguous,
  kClosed,
  kInvalidArgument,
  kPlatform,
  kBackend,
};

struct Error {
  ErrorCode code;
  std::string message;
};

template <typename T>
using Result = tl::expected<T, Error>;

}  // namespace manbo

#endif  // MANBO_RESULT_H_
