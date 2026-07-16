#pragma once

#include <string_view>

namespace eng {

// Logs (and drains) any pending OpenGL errors, tagged with `where`.
// Returns true if there were no errors. GL 4.1 has no debug callback, so
// call this at frame end (and after suspicious sections) in Debug builds.
bool check_gl_errors(std::string_view where);

}  // namespace eng
