#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/core/version.h"

int main() {
    eng::log::set_level(eng::log::Level::Debug);

    eng::log::info("FPS dedicated server starting (engine v{})", eng::version_string());
    ENG_ASSERT(eng::version_string() != nullptr, "engine must report a version");

    eng::log::info("Milestone 0 checkpoint: no simulation or networking yet.");
    eng::log::info("FPS dedicated server shutting down cleanly");
    return 0;
}
