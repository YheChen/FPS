#include "engine/rendering/gl_util.h"

#include "engine/rendering/gl.h"

#include "engine/core/log.h"

namespace eng {

namespace {

std::string_view error_name(GLenum error) {
    switch (error) {
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
        default:
            return "unknown GL error";
    }
}

}  // namespace

bool check_gl_errors(std::string_view where) {
    bool clean = true;
    while (true) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            break;
        }
        clean = false;
        log::error("GL error at {}: {} (0x{:04x})", where, error_name(error), error);
    }
    return clean;
}

}  // namespace eng
