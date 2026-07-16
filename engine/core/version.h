#pragma once

namespace eng {

// Engine version as "major.minor.patch". The value is injected by the build
// system from the CMake project version (ENG_VERSION_STRING), so there is a
// single source of truth.
const char* version_string();

}  // namespace eng
