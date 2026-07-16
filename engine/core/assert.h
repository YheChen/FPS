#pragma once

#include <cstdlib>
#include <string_view>

#include "engine/core/log.h"

// ENG_ASSERT(condition)            - aborts in Debug if condition is false.
// ENG_ASSERT(condition, "message") - same, with an extra message.
//
// Rules:
//  - Asserts document programmer invariants, never runtime error handling.
//    Network input, file I/O, and user input must NEVER be validated with
//    asserts; they use real error paths.
//  - The condition expression must be free of side effects: it is not
//    evaluated in Release builds.
namespace eng::detail {

constexpr std::string_view assert_message() {
    return {};
}
constexpr std::string_view assert_message(std::string_view message) {
    return message;
}

}  // namespace eng::detail

#if defined(ENG_ENABLE_ASSERTS)
#define ENG_ASSERT(condition, ...)                                                               \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            ::eng::log::error("Assertion failed: {} ({}:{}) {}", #condition, __FILE__, __LINE__, \
                              ::eng::detail::assert_message(__VA_ARGS__));                       \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)
#else
#define ENG_ASSERT(condition, ...) ((void)sizeof(!(condition)))
#endif
