#pragma once

// Single include point for OpenGL. Native builds use the glad loader for a
// desktop OpenGL 4.1 core context; Emscripten/WebAssembly builds use WebGL 2
// (OpenGL ES 3.0), where the browser provides the functions directly (no
// loader). Everything in engine/rendering and game/client includes THIS
// header, never <glad/gl.h> directly, so the divergence lives in one place.
#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
// Desktop-only tokens that GLES 3.0 lacks but our code references. The
// default WebGL framebuffer is already sRGB-correct, so enabling
// GL_FRAMEBUFFER_SRGB is both unnecessary and undefined here; we guard
// the call site, but define the token so shared code still compiles.
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#else
#include <glad/gl.h>
#endif

namespace eng {

// True on the WebGL 2 / GLES 3.0 path. Use for the few runtime differences
// (sRGB framebuffer enable, GLSL version directive).
inline constexpr bool kGlIsWebGL =
#if defined(__EMSCRIPTEN__)
    true;
#else
    false;
#endif

// GLSL version + precision preamble to prepend to shader sources. Desktop
// GL 4.1 uses "#version 410 core"; WebGL 2 uses "#version 300 es" and
// requires a default precision for every type it does not define one for.
// Shader bodies are otherwise written to the common subset (in/out, layout
// locations, texture()).
//
// GLSL ES has no default precision for sampler types beyond the basic
// ones, so a `sampler2DShadow` uniform fails to compile with "No precision
// specified" unless it is declared here. Desktop GLSL ignores precision
// statements, but they are only emitted on the WebGL path anyway.
//
// `sampler2D` does have a default, but it is lowp. That is fine for albedo
// and invisible on the joint texture (texelFetch of a highp-backed RGBA32F
// target), but a tangent-space normal map decoded at lowp is ~8 usable
// levels per channel, which bands every shaded gradient in the arena.
inline const char* glsl_preamble() {
    if constexpr (kGlIsWebGL) {
        return "#version 300 es\n"
               "precision highp float;\n"
               "precision highp int;\n"
               "precision highp sampler2D;\n"
               "precision highp sampler2DShadow;\n";
    } else {
        return "#version 410 core\n";
    }
}

}  // namespace eng
