#include "engine/core/version.h"

namespace eng {

const char* version_string() {
#if defined(ENG_VERSION_STRING)
    return ENG_VERSION_STRING;
#else
    return "unknown";
#endif
}

}  // namespace eng
