#include "engine/rendering/gl.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/animation/skeleton.h"
#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"
#include "engine/audio/audio_engine.h"
#include "engine/core/log.h"
#include "engine/core/time.h"
#include "engine/core/version.h"
#include "engine/debug/imgui_layer.h"
#include "engine/physics/character_controller.h"
#include "engine/physics/physics_world.h"
#include "engine/platform/input.h"
#include "engine/platform/window.h"
#include "engine/rendering/camera.h"
#include "engine/rendering/debug_draw.h"
#include "engine/rendering/gl_util.h"
#include "engine/rendering/gpu_mesh.h"
#include "engine/rendering/joint_texture.h"
#include "engine/rendering/light.h"
#include "engine/rendering/particle_renderer.h"
#include "engine/rendering/particle_sim.h"
#include "engine/rendering/postfx.h"
#include "engine/rendering/screenshot.h"
#include "engine/rendering/shader.h"
#include "engine/rendering/shadow_map.h"
#include "engine/rendering/sky.h"
#include "engine/rendering/texture.h"
#include "engine/scene/scene.h"

#include "game/client/fly_camera.h"
#include "game/client/net_client.h"
#include "game/shared/footsteps.h"
#include "game/shared/health.h"
#include "game/shared/hitscan.h"
#include "game/shared/input_command.h"
#include "game/shared/interpolation.h"
#include "game/shared/player_color.h"
#include "game/shared/player_movement.h"
#include "game/shared/prediction.h"
#include "game/shared/replay.h"
#include "game/shared/rng.h"
#include "game/shared/weapon.h"

namespace {

#if defined(__EMSCRIPTEN__)
// Telemetry for the automated browser check (tools/web_smoke.mjs, run by the
// `web` CI job). Every web-only defect this project has shipped was invisible
// to the compiler -- a GLSL ES precision error, a 0x0 startup canvas, a WebGL
// uniform-indexing cliff that put one draw call at 1.5 s -- and all three
// looked identical from outside: the page loads, and then nothing moves. So
// the check watches frames go by, and a fingerprint of the pixels proves the
// frames contain a scene rather than a clear colour.
//
// Web-only on purpose. Natively the equivalent evidence is --screenshot, and
// there is no reason to carry a JS bridge into the desktop build.
// clang-format off
// The body is JavaScript, and clang-format reads it as C++: it turns the
// object literal's `frames:` into a `frames :` label. Still valid JS, but it
// reads like a typo, so this one block opts out.
EM_JS(void, fps_publish_smoke, (int frames, double seconds, int distinct_colors, double mean_luma), {
    Module.fpsSmoke = {
        frames: frames,
        seconds: seconds,
        distinctColors: distinct_colors,
        meanLuma: mean_luma,
    };
});
// clang-format on

// Which frame to fingerprint. Late enough that the first frame's warm-up is
// over, early enough that a smoke run does not have to be long: even the
// software rasteriser CI renders with clears this in a couple of seconds.
constexpr int kSmokeSignatureFrame = 30;
#endif

// Shaders omit #version; Shader::create prepends the platform preamble
// (desktop GLSL 410 core vs WebGL2 GLSL ES 300).
// Skinning is shared by the lit and depth shaders, so the two passes cannot
// disagree about where a vertex is -- a skinned mesh that casts an unskinned
// shadow is a very confusing bug to look at.
//
// 32 joints is 128 vec4 of uniform space. GL 4.1 and WebGL 2 both guarantee
// at least 256 vertex uniform vectors and the rest of this shader uses ~15,
// so a plain uniform array is enough; a UBO would only be needed for a much
// larger rig.
constexpr std::string_view kSkinningGlsl = R"(
layout(location = 3) in uvec4 a_joints;
layout(location = 4) in vec4 a_weights;
uniform sampler2D u_joint_texture;   // one matrix per row, 4 RGBA texels wide
uniform bool u_skinned;

mat4 joint_matrix(uint index) {
    int row = int(index);
    return mat4(texelFetch(u_joint_texture, ivec2(0, row), 0),
                texelFetch(u_joint_texture, ivec2(1, row), 0),
                texelFetch(u_joint_texture, ivec2(2, row), 0),
                texelFetch(u_joint_texture, ivec2(3, row), 0));
}

// Returns the matrix to apply before u_model. Static geometry carries all
// weights at 0, so it must fall through to the identity rather than
// accumulating nothing and collapsing to the origin.
//
// Joint matrices arrive in a texture, not a uniform array: dynamically
// indexing a uniform array is a severe WebGL 2 performance cliff (see
// joint_texture.h).
mat4 skin_matrix() {
    if (!u_skinned) {
        return mat4(1.0);
    }
    float total = a_weights.x + a_weights.y + a_weights.z + a_weights.w;
    if (total <= 0.0) {
        return mat4(1.0);
    }
    mat4 skin = a_weights.x * joint_matrix(a_joints.x) +
                a_weights.y * joint_matrix(a_joints.y) +
                a_weights.z * joint_matrix(a_joints.z) +
                a_weights.w * joint_matrix(a_joints.w);
    // Renormalize rather than trusting the asset: weights that sum to 0.98
    // would shrink the mesh slightly, which reads as a wobble in motion.
    return skin / total;
}
)";

constexpr std::string_view kLitVertexSource = R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 5) in vec4 a_tangent;   // xyz direction, w handedness
uniform mat4 u_model;
uniform mat3 u_normal_matrix;
uniform mat4 u_view_projection;
out vec3 v_world_pos;
out vec3 v_normal;
out vec2 v_uv;
out vec4 v_tangent;
void main() {
    mat4 skin = skin_matrix();
    vec4 world = u_model * skin * vec4(a_position, 1.0);
    v_world_pos = world.xyz;
    // The joint rotation has to reach the normal too, or a skinned limb keeps
    // the lighting of its bind pose. mat3 of the skin matrix is close enough
    // here: joints are rigid, with no non-uniform scale to invert-transpose.
    v_normal = u_normal_matrix * (mat3(skin) * a_normal);
    // Same transform for the tangent, and the handedness rides through
    // untouched: it is a property of the UV layout, not of the pose. A zero
    // tangent stays zero through both matrices, which is what lets the
    // fragment stage recognise geometry with no tangent frame.
    v_tangent = vec4(u_normal_matrix * (mat3(skin) * a_tangent.xyz), a_tangent.w);
    v_uv = a_uv;
    gl_Position = u_view_projection * world;
}
)";

constexpr std::string_view kLitFragmentSource = R"(
in vec3 v_world_pos;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_tangent;
uniform sampler2D u_base_color;
uniform sampler2D u_normal_map;
uniform float u_normal_strength;   // 0 disables the map without a rebind
uniform sampler2DShadow u_shadow_map;
uniform mat4 u_light_view_projection;
uniform float u_shadow_texel;   // 1.0 / shadow map resolution
uniform vec3 u_tint;
uniform vec3 u_light_direction;
uniform vec3 u_light_color;
uniform vec3 u_ambient;
uniform vec3 u_camera_pos;
out vec4 o_color;

// Returns 1.0 in full light, 0.0 in full shadow. 3x3 PCF on top of the
// hardware's own 2x2 comparison filtering.
float sun_visibility(vec3 normal, vec3 to_light) {
    vec4 light_clip = u_light_view_projection * vec4(v_world_pos, 1.0);
    vec3 proj = light_clip.xyz / light_clip.w * 0.5 + 0.5;

    // Outside the light's frustum (or past its far plane) means unshadowed:
    // the map simply has no information there.
    if (proj.z > 1.0 || any(lessThan(proj.xy, vec2(0.0))) ||
        any(greaterThan(proj.xy, vec2(1.0)))) {
        return 1.0;
    }

    // Slope-scaled bias: surfaces edge-on to the light span more depth per
    // texel, so they need more slack before a texel shadows itself.
    float slope = clamp(1.0 - dot(normal, to_light), 0.0, 1.0);
    float bias = 0.0012 + 0.0045 * slope;

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * u_shadow_texel;
            sum += texture(u_shadow_map, vec3(proj.xy + offset, proj.z - bias));
        }
    }
    return sum / 9.0;
}

// The shading normal: the normal map rotated into world space, or just the
// interpolated vertex normal where there is no tangent frame to rotate it
// with.
//
// Most of the meshes in this game take the second branch -- the character,
// the targets, anything untextured -- so it has to be the safe one. The
// guard is on the length of the ORTHOGONALIZED tangent, which catches both
// "this mesh has no tangents at all" (zero) and "interpolation left the
// tangent parallel to the normal" (also zero after projection). Normalizing
// either would produce a NaN and paint the surface black.
vec3 shading_normal(vec3 n) {
    // Sampled BEFORE the guard, deliberately. texture() picks its mip level
    // from screen-space derivatives, and GLSL ES 3.00 leaves those undefined
    // inside non-uniform control flow -- which the branch below becomes the
    // moment a 2x2 quad straddles a vertex whose tangent is degenerate. No
    // mesh mixes the two today, so nothing is broken now; it would start
    // being broken the first time one does (a UV seam collapsed to a point,
    // or a viewmodel handed tangents by the loader), and the symptom would
    // be a speckled seam on some drivers and not others. Hoisting costs one
    // fetch of the 1x1 flat-normal texture on geometry that then discards it.
    vec3 tangent_normal = texture(u_normal_map, v_uv).xyz * 2.0 - 1.0;

    vec3 t = v_tangent.xyz - n * dot(n, v_tangent.xyz);
    if (dot(t, t) < 1e-12) {
        return n;
    }
    t = normalize(t);
    vec3 b = cross(n, t) * v_tangent.w;
    tangent_normal.xy *= u_normal_strength;
    return normalize(mat3(t, b, n) * tangent_normal);
}

void main() {
    vec3 albedo = texture(u_base_color, v_uv).rgb * u_tint;
    vec3 geometric = normalize(v_normal);
    vec3 n = shading_normal(geometric);
    vec3 l = normalize(-u_light_direction);
    float n_dot_l = max(dot(n, l), 0.0);
    vec3 view_dir = normalize(u_camera_pos - v_world_pos);
    vec3 half_dir = normalize(l + view_dir);
    float spec = pow(max(dot(n, half_dir), 0.0), 32.0) * 0.25;

    // Ambient stays unshadowed; only the sun's direct contribution is
    // occluded, which is what keeps shadowed areas readable rather than
    // black.
    //
    // The shadow bias is computed from the GEOMETRIC normal, not the mapped
    // one: it is slack for how much depth the surface's own footprint spans
    // in a shadow texel, which is a property of the triangle. Feeding it the
    // bumpy normal would modulate the bias per texel and speckle flat walls
    // with acne.
    float visibility = sun_visibility(geometric, l);
    vec3 direct = u_light_color * n_dot_l * visibility;
    vec3 color = albedo * (u_ambient + direct) + u_light_color * spec * n_dot_l * visibility;
    o_color = vec4(color, 1.0);
}
)";

// Depth-only pass from the light's point of view. No fragment work beyond
// the implicit depth write.
constexpr std::string_view kDepthVertexSource = R"(
layout(location = 0) in vec3 a_position;
uniform mat4 u_model;
uniform mat4 u_light_view_projection;
void main() {
    gl_Position = u_light_view_projection * u_model * skin_matrix() * vec4(a_position, 1.0);
}
)";

constexpr std::string_view kDepthFragmentSource = R"(
void main() {}
)";

struct ClientArgs {
    std::optional<double> run_seconds;
    std::optional<std::string> connect_host;  // online mode when set
    std::uint16_t port = 7777;
    std::string name = "player";
    bool vsync = true;          // --no-vsync: automated multi-window runs must not
                                // block on swap (macOS throttles occluded windows)
    eng::NetSimConfig net_sim;  // --fake-latency/--fake-jitter/--fake-loss
    // Automated-verification hooks (harmless in normal play):
    bool auto_fire = false;                  // pull the trigger every other tick
    bool auto_walk = false;                  // hold W: bob needs distance actually travelled
    std::optional<std::uint8_t> weapon;      // start on this slot (1-based, as the keys are)
    std::optional<float> fixed_yaw;          // lock the view yaw (radians)
    std::optional<float> fixed_pitch;        // lock the view pitch (radians, + is up)
    std::optional<std::string> screenshot;   // PNG written on the final frame
    std::optional<std::string> replay_path;  // --replay: watch a recording
    bool audio_trace = false;                // dump every spatialized sound
    // Geometry is loaded once, before the menu, because the menu orbits it.
    // So the map has to be chosen before the server can be asked which one it
    // is running; a mismatch is caught at the welcome instead.
    std::string map = "maps/arena01.glb";  // --map
};

#if defined(__EMSCRIPTEN__)
// A browser has no argv, so connection details come from the query string:
//   fps_client.html?connect=rtc://host:port&name=bob
// That doubles as a shareable join link for a deployed build, and it is the
// only way to drive the client without clicking through the menu -- which is
// what makes an automated browser test of the connection path possible.
// clang-format off
EM_JS(int, fps_read_query_param, (const char* key, char* out, int max), {
    var params = new URLSearchParams(window.location.search);
    var value = params.get(UTF8ToString(key));
    if (!value) { return 0; }
    stringToUTF8(value, out, max);
    return 1;
});
// clang-format on

std::optional<std::string> query_param(const char* key) {
    constexpr int kMax = 256;
    std::string buffer(kMax, '\0');
    if (fps_read_query_param(key, buffer.data(), kMax) == 0) {
        return std::nullopt;
    }
    return std::string(buffer.c_str());
}
#endif

ClientArgs parse_args(int argc, char** argv) {
    ClientArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next_value = [&]() -> std::optional<std::string_view> {
            if (i + 1 < argc) {
                return std::string_view{argv[++i]};
            }
            return std::nullopt;
        };
        if (arg == "--run-seconds") {
            if (const auto value = next_value()) {
                double seconds = 0.0;
                if (std::from_chars(value->data(), value->data() + value->size(), seconds).ec ==
                    std::errc{}) {
                    args.run_seconds = seconds;
                }
            }
        } else if (arg == "--connect") {
            if (const auto value = next_value()) {
                args.connect_host = std::string(*value);
            }
        } else if (arg == "--port") {
            if (const auto value = next_value()) {
                std::uint16_t port = 0;
                if (std::from_chars(value->data(), value->data() + value->size(), port).ec ==
                    std::errc{}) {
                    args.port = port;
                }
            }
        } else if (arg == "--name") {
            if (const auto value = next_value()) {
                args.name = std::string(*value);
            }
        } else if (arg == "--map") {
            if (const auto value = next_value()) {
                args.map = std::string(*value);
            }
        } else if (arg == "--no-vsync") {
            args.vsync = false;
        } else if (arg == "--screenshot") {
            if (const auto value = next_value()) {
                args.screenshot = std::string(*value);
            }
        } else if (arg == "--replay") {
            if (const auto value = next_value()) {
                args.replay_path = std::string(*value);
            }
        } else if (arg == "--auto-fire") {
            args.auto_fire = true;
        } else if (arg == "--auto-walk") {
            args.auto_walk = true;
        } else if (arg == "--weapon") {
            if (const auto value = next_value()) {
                std::uint8_t slot = 0;
                if (std::from_chars(value->data(), value->data() + value->size(), slot).ec ==
                        std::errc{} &&
                    slot >= 1) {
                    args.weapon = static_cast<std::uint8_t>(slot - 1);
                }
            }
        } else if (arg == "--audio-trace") {
            args.audio_trace = true;
        } else if (arg == "--fixed-yaw") {
            if (const auto value = next_value()) {
                float yaw = 0.0f;
                if (std::from_chars(value->data(), value->data() + value->size(), yaw).ec ==
                    std::errc{}) {
                    args.fixed_yaw = yaw;
                }
            }
        } else if (arg == "--fixed-pitch") {
            if (const auto value = next_value()) {
                float pitch = 0.0f;
                if (std::from_chars(value->data(), value->data() + value->size(), pitch).ec ==
                    std::errc{}) {
                    args.fixed_pitch = pitch;
                }
            }
        } else if (arg == "--fake-latency") {
            if (const auto value = next_value()) {
                std::from_chars(value->data(), value->data() + value->size(),
                                args.net_sim.latency_ms);
            }
        } else if (arg == "--fake-jitter") {
            if (const auto value = next_value()) {
                std::from_chars(value->data(), value->data() + value->size(),
                                args.net_sim.jitter_ms);
            }
        } else if (arg == "--fake-loss") {
            if (const auto value = next_value()) {
                std::from_chars(value->data(), value->data() + value->size(),
                                args.net_sim.loss_percent);
            }
        } else {
            eng::log::warn("Unknown argument '{}'", arg);
        }
    }
#if defined(__EMSCRIPTEN__)
    // After the argv loop so a hand-built command line still wins where one
    // exists; in a browser there is none, so these are the only source.
    if (const auto connect = query_param("connect")) {
        args.connect_host = *connect;
    }
    if (const auto name = query_param("name")) {
        args.name = *name;
    }
#endif
    return args;
}

struct RenderPrimitive {
    eng::GpuMesh gpu;
    glm::vec3 color{1.0f};
    int texture = -1;         // index into the arena texture list, -1 = untextured
    int normal_texture = -1;  // same list, -1 = no normal map
    float normal_scale = 1.0f;
};

// Upper bound on rig size; the joint texture is sized to this.
constexpr std::size_t kMaxJoints = 32;

// One drawable submission, shared by the shadow pass and the lit pass.
enum class DrawKind : std::uint8_t {
    ArenaMesh,  // `mesh` indexes render_meshes
    Cube,       // the unit cube (targets)
    Character,  // the skinned figure, posed by `joint_offset`
};

struct DrawItem {
    glm::mat4 model{1.0f};
    DrawKind kind = DrawKind::Cube;
    int mesh = -1;
    glm::vec3 tint{1.0f};
    // Slice into the frame's joint-matrix pool. Both passes read the same
    // slice, so a character cannot be posed differently for its shadow.
    int joint_offset = -1;
    int joint_count = 0;
};

// Shortest signed step from `from` to `to`, in (-pi, pi]. bot.h has the
// canonical copy, but the client has no business pulling in bot
// decision-making for three lines of angle maths. Interpolating yaw without
// this spins the long way round whenever a sample pair straddles +/-pi, which
// on screen reads as the camera glitching rather than as an angle wrap.
float shortest_yaw_delta(float from, float to) {
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    float delta = std::fmod(to - from + std::numbers::pi_v<float>, kTwoPi);
    if (delta < 0.0f) {
        delta += kTwoPi;
    }
    return delta - std::numbers::pi_v<float>;
}

// Killcam playback rate. Must match the rate the server SAMPLES at
// (kKillCamTickStride against the 60 Hz tick), or the replay runs fast or
// slow and the moment of the shot lands somewhere other than the end.
constexpr float kKillCamPlaybackHz = 60.0f / static_cast<float>(game::kKillCamTickStride);

// One replayed player: the same PlayerState and controller the server uses,
// stepped by the recorded inputs. Positions were never recorded, so what is
// on screen is genuinely re-simulated rather than played back.
struct ReplayActor {
    bool active = false;
    std::string name;
    game::PlayerState state;
    // Facing is not part of PlayerState (the server keeps it on the command),
    // so it is carried alongside for rendering.
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::unique_ptr<eng::CharacterController> controller;
};

// Playback cursor over a loaded replay.
struct ReplayPlayback {
    game::Replay replay;
    std::array<ReplayActor, game::kMaxPlayers> actors;
    std::size_t next_frame = 0;
    bool paused = false;
    float speed = 1.0f;
    // Fractional frames carried between ticks so speeds below 1x work
    // without stuttering.
    float frame_debt = 0.0f;
    int follow = -1;  // player id to chase, -1 = free camera

    bool finished() const { return next_frame >= replay.frames.size(); }
    std::uint32_t current_tick() const {
        return next_frame == 0 ? 0u : replay.frames[next_frame - 1].tick;
    }
};

// Rebuilds every actor at its recorded spawn and rewinds to the first frame.
void reset_playback(ReplayPlayback& playback, eng::PhysicsWorld& world) {
    for (ReplayActor& actor : playback.actors) {
        actor = ReplayActor{};
    }
    for (const game::ReplayPlayer& recorded : playback.replay.players) {
        ReplayActor& actor = playback.actors[recorded.id];
        actor.active = true;
        actor.name = recorded.name;
        actor.state = game::PlayerState{};
        actor.state.position = recorded.spawn;
        actor.controller = std::make_unique<eng::CharacterController>(world, recorded.spawn);
    }
    playback.next_frame = 0;
    playback.frame_debt = 0.0f;
}

// Applies one recorded frame through the SAME advance_player the server runs.
void step_playback(ReplayPlayback& playback, eng::PhysicsWorld& world) {
    if (playback.finished()) {
        return;
    }
    const game::ReplayFrame& frame = playback.replay.frames[playback.next_frame++];
    for (const game::ReplayCommand& entry : frame.commands) {
        ReplayActor& actor = playback.actors[entry.player_id];
        if (!actor.active) {
            continue;  // a command for a player the header never declared
        }
        actor.yaw = entry.command.yaw;
        actor.pitch = entry.command.pitch;
        game::advance_player(actor.state, entry.command, game::kTickSeconds, *actor.controller,
                             world);
    }
}

// Animation state for one figure. Cosmetic, like particles: driven by the
// render clock and never fed back into the simulation.
struct CharacterAnimation {
    float clip_time = 0.0f;   // position within idle/run
    float run_weight = 0.0f;  // 0 = idle, 1 = run
    float air_time = 0.0f;    // position within the jump clip
    bool airborne = false;
};

// Blends toward running with horizontal speed and switches to the jump clip
// off the ground. `run_speed` is the speed at which the run blend saturates.
void update_character_animation(CharacterAnimation& animation, const glm::vec3& velocity,
                                bool on_ground, float run_speed, float dt) {
    const float horizontal = glm::length(glm::vec2{velocity.x, velocity.z});
    const float target = glm::clamp(horizontal / std::max(run_speed, 0.01f), 0.0f, 1.0f);
    // Ease toward the target so a stop does not snap mid-stride.
    animation.run_weight += (target - animation.run_weight) * std::min(1.0f, dt * 9.0f);

    // The cycle advances with speed, so the feet do not appear to slide: a
    // fixed playback rate looks wrong at every speed but one.
    const float rate = glm::mix(1.0f, std::max(1.0f, horizontal / 3.0f), animation.run_weight);
    animation.clip_time += dt * rate;

    animation.airborne = !on_ground;
    animation.air_time = on_ground ? 0.0f : animation.air_time + dt;
}

// 2048^2 over a ~45 m arena is roughly 2 cm per texel, which is enough for
// crisp pillar and player shadows without a cascade split.
constexpr int kShadowResolution = 2048;

// Enough for a shotgun blast (8 impacts) plus several older bursts still
// fading. Bursts are clipped rather than queued when it fills.
constexpr std::size_t kMaxParticles = 4096;

// --- particle effects ----------------------------------------------------
// Colors are premultiplied alpha: alpha 0 with bright RGB is additive glow,
// alpha > 0 is an opaque puff. See ParticleRenderer.

void emit_muzzle_flash(eng::ParticlePool& pool, const glm::vec3& position,
                       const glm::vec3& direction, std::uint32_t seed) {
    // Deliberately small and slow. The muzzle sits ~0.5 m from the near
    // plane, so world-space sizes and speeds that look right out in the
    // arena fill the screen with drifting orbs here.
    eng::EmitParams params;
    params.position = position;
    params.direction = direction;
    params.cone_radians = 0.34f;
    params.speed = 1.1f;
    params.speed_jitter = 0.5f;
    params.color_start = {2.4f, 1.7f, 0.7f, 0.0f};  // additive, over-bright
    params.color_end = {0.5f, 0.2f, 0.0f, 0.0f};
    params.size_start = 0.055f;
    params.size_end = 0.005f;
    params.lifetime_seconds = 0.05f;
    params.lifetime_jitter = 0.3f;
    params.drag = 12.0f;
    params.count = 8;
    pool.emit(params, seed);
}

void emit_impact(eng::ParticlePool& pool, const glm::vec3& position, const glm::vec3& normal,
                 std::uint32_t seed) {
    // Sparks fly back along the surface normal and fall.
    eng::EmitParams sparks;
    sparks.position = position;
    sparks.direction = normal;
    sparks.cone_radians = 0.9f;
    sparks.speed = 4.5f;
    sparks.speed_jitter = 0.7f;
    sparks.color_start = {2.0f, 1.1f, 0.35f, 0.0f};
    sparks.color_end = {0.6f, 0.15f, 0.0f, 0.0f};
    sparks.size_start = 0.035f;
    sparks.size_end = 0.008f;
    sparks.lifetime_seconds = 0.32f;
    sparks.lifetime_jitter = 0.5f;
    sparks.gravity = 11.0f;
    sparks.drag = 1.4f;
    sparks.count = 10;
    pool.emit(sparks, seed);

    // A slower dust puff, opaque so it reads against a bright wall.
    eng::EmitParams dust;
    dust.position = position;
    dust.direction = normal;
    dust.cone_radians = 1.2f;
    dust.speed = 0.9f;
    dust.speed_jitter = 0.6f;
    dust.color_start = {0.34f, 0.32f, 0.30f, 0.42f};
    dust.color_end = {0.0f, 0.0f, 0.0f, 0.0f};
    dust.size_start = 0.10f;
    dust.size_end = 0.34f;
    dust.lifetime_seconds = 0.5f;
    dust.lifetime_jitter = 0.4f;
    dust.gravity = -1.2f;  // negative: the puff drifts upward as it thins
    dust.drag = 3.0f;
    dust.count = 7;
    pool.emit(dust, seed ^ 0x5bf03635u);
}

void emit_blood(eng::ParticlePool& pool, const glm::vec3& position, const glm::vec3& direction,
                std::uint32_t seed) {
    eng::EmitParams params;
    params.position = position;
    params.direction = direction;
    params.cone_radians = 1.0f;
    params.speed = 3.2f;
    params.speed_jitter = 0.7f;
    params.color_start = {0.42f, 0.02f, 0.03f, 0.85f};
    params.color_end = {0.10f, 0.0f, 0.0f, 0.0f};
    params.size_start = 0.07f;
    params.size_end = 0.02f;
    params.lifetime_seconds = 0.45f;
    params.lifetime_jitter = 0.4f;
    params.gravity = 9.0f;
    params.drag = 2.2f;
    params.count = 14;
    pool.emit(params, seed);
}

void emit_death_burst(eng::ParticlePool& pool, const glm::vec3& position, std::uint32_t seed) {
    eng::EmitParams params;
    params.position = position;
    params.direction = {0.0f, 1.0f, 0.0f};
    params.cone_radians = 1.55f;  // very close to a full hemisphere
    params.speed = 6.0f;
    params.speed_jitter = 0.8f;
    params.color_start = {0.55f, 0.03f, 0.04f, 0.9f};
    params.color_end = {0.08f, 0.0f, 0.0f, 0.0f};
    params.size_start = 0.11f;
    params.size_end = 0.03f;
    params.lifetime_seconds = 0.8f;
    params.lifetime_jitter = 0.5f;
    params.gravity = 10.0f;
    params.drag = 1.1f;
    params.count = 46;
    pool.emit(params, seed);
}

// Direction the sun's light travels.
constexpr glm::vec3 kSunDirection{-0.4f, -1.0f, -0.3f};

// --- first-person viewmodel ------------------------------------------------
// The held weapon, drawn from the camera basis. Nothing here is in world
// space and nothing here is simulated: like particles and the character
// animation above, the viewmodel runs on the RENDER clock and must never
// feed back into the fixed tick, or two clients would disagree about where a
// shot went because one of them had a better frame rate.

// One weapon's geometry as gen_weapons.py emits it: a handful of boxes, each
// a node with its own grip-local transform, over one glTF mesh per material.
struct WeaponModel {
    // Which primitives to draw and where. `primitive` indexes `primitives`.
    struct Box {
        glm::mat4 local{1.0f};
        int primitive = 0;
    };
    std::vector<RenderPrimitive> primitives;
    std::vector<Box> boxes;
    // Grip-local muzzle, from the asset's `muzzle` marker node. Absent on a
    // melee weapon, and that absence is what stops a knife from spitting
    // fire: the flash position is a property of the ASSET, so four weapons of
    // four different lengths need no `if (weapon == ...)` in the client.
    std::optional<glm::vec3> muzzle;
    // How far the geometry reaches forward of the grip. The arm extends by
    // whatever this leaves over, so every weapon presents its business end
    // at the same distance -- see kViewmodelBusinessEnd.
    float reach = 0.0f;

    bool loaded() const { return !boxes.empty(); }
};

// Where the grip sits relative to the eye, in camera-basis meters. Right and
// down are fixed for the whole arsenal -- the shoulder does not move when you
// swap guns. The DROP is the one that was tuned hardest, because it is what
// decides how much of the frame the weapon eats: it has to pay for the bore
// sitting 0.105 above the grip on every gun AND for the sniper's scope, which
// rides 0.250 above it. At a 0.25 drop the scope sat exactly on the eye line
// and the optic covered the right half of the screenshot -- measured, not
// guessed. 0.34 puts the tallest thing in the arsenal below the crosshair
// with the barrel still climbing toward it.
constexpr float kViewmodelRight = 0.20f;
constexpr float kViewmodelDown = 0.34f;

// Forward is NOT fixed, because the arm is not: a short weapon is presented
// out in front and a long one is drawn back against the shoulder, which is
// one rule for the arsenal rather than five numbers. The arm extends until
// the weapon's business end is this far from the eye...
constexpr float kViewmodelBusinessEnd = 0.76f;
// ...but only as far as a shoulder and an elbow allow. The near clamp is
// where all three long guns end up, so it is the load-bearing number here:
// the rule alone would hold the 1.30 m sniper's grip 0.10 m from the eye, and
// geometry that close subtends the frame no matter how modest it is in
// meters. Only the knife (0.58 m) and the smg (0.43 m) come out above it.
// Nothing about a weapon should make it bigger than the arena it is carried
// through.
constexpr float kViewmodelForwardMin = 0.42f;
constexpr float kViewmodelForwardMax = 0.60f;

// Near plane for the viewmodel's own projection. See the draw site: the gun
// is closer than the world's 0.05 m near plane in places.
constexpr float kViewmodelNear = 0.01f;
constexpr float kViewmodelFar = 10.0f;

// Bob. Distance ACTUALLY TRAVELLED, not a timer and not speed x dt: a timer
// is a cadence that is right at exactly one speed, and speed x dt keeps
// accumulating while you hold W against a wall, where the player is going
// nowhere and the gun should be still. The stride is the same 2.4 m a
// footstep uses, so the gun's dip lands ON the footfall rather than drifting
// against it; the lateral swing runs at half that rate, so one left-right
// cycle covers a pair of steps.
constexpr float kBobStrideMeters = 2.4f;
// A frame that moves the player further than this is a respawn or a
// reconciliation snap, not a stride, and must not spin the accumulator.
constexpr float kBobMaxStepMeters = 1.0f;
constexpr float kBobLateral = 0.016f;
constexpr float kBobVertical = 0.011f;

// Recoil, as a spring: an impulse on fire and a decay that overshoots
// slightly coming back, which is what reads as the gun settling rather than
// sliding home. Stiffness/damping give zeta ~0.74 and a ~0.3 s settle.
constexpr float kRecoilStiffness = 220.0f;
constexpr float kRecoilDamping = 22.0f;
constexpr float kRecoilImpulse = 26.0f;
// Payload per trigger pull (damage x pellets) against the sniper's, which is
// what makes the config's own numbers decide how hard a weapon kicks:
// smg 0.19, rifle 0.33, sniper 1.00, shotgun 1.17. Nothing here is tuned per
// weapon, so a new .cfg gets a kick that matches its stats for free.
constexpr float kRecoilReferenceDamage = 75.0f;
constexpr float kRecoilPitchRadians = 0.10f;  // muzzle rise about the grip
constexpr float kRecoilBackMeters = 0.05f;    // and a shove toward the eye
constexpr float kRecoilYawRadians = 0.022f;   // alternating, so bursts wander
// A melee weapon has no muzzle to rise: the same impulse drives a slash
// across the view instead.
constexpr float kSwingYawRadians = 0.55f;
constexpr float kSwingRollRadians = 0.50f;
constexpr float kSwingLungeMeters = 0.10f;

// Sway. The gun catches up to the aim over ~0.1 s; the leftover gap is the
// lag, and it is clamped so a fast flick trails the view instead of leaving
// the frame.
constexpr float kSwayCatchup = 11.0f;
constexpr float kSwayMaxRadians = 0.30f;
constexpr float kSwayMeters = 0.16f;
constexpr float kSwayRotate = 0.45f;

// Reload: the weapon drops out of the aim and tips over so the magazine well
// faces the camera, then comes back. Peaks halfway through reload_seconds.
constexpr float kReloadDropMeters = 0.10f;
constexpr float kReloadPitchRadians = 0.55f;
constexpr float kReloadRollRadians = 0.35f;

// Switch: the new weapon comes up from below over switch_seconds. Quadratic,
// so it leaves fast and eases into the aim -- which is what makes the
// knife's 0.15 s feel like a flick and the sniper's 0.7 s feel like work.
constexpr float kRaiseDropMeters = 0.34f;
constexpr float kRaisePitchRadians = 0.9f;

// Everything the viewmodel remembers between frames.
struct ViewmodelState {
    // Which weapon is drawn. 0xFF means "nothing yet", so the first frame
    // raises the starting weapon instead of silently skipping the animation.
    std::uint8_t slot = 0xFF;
    float raise_remaining = 0.0f;
    float raise_seconds = 0.0f;
    float reload_remaining = 0.0f;
    float reload_seconds = 0.0f;
    bool was_reloading = false;
    float recoil = 0.0f;
    float recoil_velocity = 0.0f;
    int shots = 0;  // parity only, so consecutive kicks alternate sideways
    float bob_distance = 0.0f;
    float bob_weight = 0.0f;
    glm::vec3 last_position{0.0f};
    bool position_valid = false;
    // The aim the gun has caught up to. The gap to the camera's is the lag.
    float sway_yaw = 0.0f;
    float sway_pitch = 0.0f;
    bool sway_valid = false;
};

// Kicks the weapon on a shot. The impulse is the ONLY thing the weapon's
// stats feed, and it deliberately does not touch the camera: aim punch would
// change where bullets go, which is simulation, not decoration.
void fire_viewmodel(ViewmodelState& state, const game::WeaponConfig& config) {
    const float payload = config.damage * static_cast<float>(config.pellets);
    state.recoil_velocity += kRecoilImpulse * payload / kRecoilReferenceDamage;
    ++state.shots;
}

// Advances the viewmodel one render frame. `reloading` is a level rather than
// an edge because that is all the online path has (NetClient reports a bool,
// not a remaining time); taking the edge here and running the clip off the
// config's own reload_seconds gives offline and online the same animation
// from the same number.
void update_viewmodel(ViewmodelState& state, const game::WeaponConfig& config, std::uint8_t slot,
                      bool reloading, const glm::vec3& position, const glm::vec3& velocity,
                      bool on_ground, float run_speed, float yaw, float pitch, float dt) {
    if (slot != state.slot) {
        state.slot = slot;
        state.raise_seconds = std::max(config.switch_seconds, 1e-3f);
        state.raise_remaining = state.raise_seconds;
        // Switching interrupts a reload, exactly as update_loadout does; the
        // level is latched so the new weapon does not inherit the old one's
        // in-progress clip on its first frame.
        state.reload_remaining = 0.0f;
        state.was_reloading = reloading;
        state.recoil = 0.0f;
        state.recoil_velocity = 0.0f;
    }
    state.raise_remaining = std::max(0.0f, state.raise_remaining - dt);

    // reload_seconds is 0 for a melee weapon, so the knife can never start a
    // reload clip even if something upstream claimed it was reloading.
    if (reloading && !state.was_reloading && config.reload_seconds > 0.0f) {
        state.reload_seconds = config.reload_seconds;
        state.reload_remaining = config.reload_seconds;
    }
    state.was_reloading = reloading;
    state.reload_remaining = std::max(0.0f, state.reload_remaining - dt);

    // Clamped so a frame spike cannot integrate the spring into a divergence;
    // 20 Hz is still an order of magnitude inside its stability limit.
    const float spring_dt = std::min(dt, 0.05f);
    state.recoil_velocity +=
        (-kRecoilStiffness * state.recoil - kRecoilDamping * state.recoil_velocity) * spring_dt;
    state.recoil += state.recoil_velocity * spring_dt;

    glm::vec3 step = position - state.last_position;
    step.y = 0.0f;  // a fall is not a stride
    float travelled = glm::length(step);
    if (!state.position_valid || travelled > kBobMaxStepMeters) {
        travelled = 0.0f;
    }
    state.last_position = position;
    state.position_valid = true;
    if (on_ground) {
        state.bob_distance += travelled;
    }
    // Amplitude still comes from velocity rather than from that step: the
    // simulation only moves on fixed ticks, so at 300 fps four frames in five
    // travel exactly zero and a weight driven off them would flicker.
    const float speed = glm::length(glm::vec2{velocity.x, velocity.z});
    // Off the ground there is no stride to be in phase with, so the bob eases
    // out rather than freezing mid-swing.
    const float target =
        on_ground ? glm::clamp(speed / std::max(run_speed, 0.01f), 0.0f, 1.0f) : 0.0f;
    state.bob_weight += (target - state.bob_weight) * std::min(1.0f, dt * 8.0f);

    if (!state.sway_valid) {
        state.sway_yaw = yaw;
        state.sway_pitch = pitch;
        state.sway_valid = true;
    }
    // Shortest-way, or a turn across +-pi would send the gun the long way
    // round the player's head.
    state.sway_yaw += shortest_yaw_delta(state.sway_yaw, yaw) * std::min(1.0f, dt * kSwayCatchup);
    state.sway_pitch += (pitch - state.sway_pitch) * std::min(1.0f, dt * kSwayCatchup);
}

// Builds the weapon's world transform from the camera basis: local +X is the
// camera's right, +Y its up, -Z where it is looking, and the origin is the
// grip. Bob, sway, recoil, reload and raise are all offsets and rotations
// ABOUT THE GRIP on top of that, which is why every weapon can share them --
// the grip is the one point all five models agree on.
glm::mat4 viewmodel_transform(const ViewmodelState& state, const eng::Camera& camera, float reach,
                              bool melee) {
    const glm::vec3 forward = camera.forward();
    const glm::vec3 right = camera.right();
    const glm::vec3 up = glm::cross(right, forward);

    glm::vec3 offset{
        kViewmodelRight, -kViewmodelDown,
        glm::clamp(kViewmodelBusinessEnd - reach, kViewmodelForwardMin, kViewmodelForwardMax)};
    float pitch_kick = 0.0f;
    float yaw_kick = 0.0f;
    float roll_kick = 0.0f;

    const float bob_phase = state.bob_distance / kBobStrideMeters * std::numbers::pi_v<float>;
    offset.x += std::sin(bob_phase) * kBobLateral * state.bob_weight;
    // abs() halves the period against the lateral swing and puts a hard
    // bottom on the dip, which is the frame the foot lands.
    offset.y -= std::abs(std::sin(bob_phase)) * kBobVertical * state.bob_weight;

    const float lag_yaw = glm::clamp(shortest_yaw_delta(state.sway_yaw, camera.yaw),
                                     -kSwayMaxRadians, kSwayMaxRadians);
    const float lag_pitch =
        glm::clamp(camera.pitch - state.sway_pitch, -kSwayMaxRadians, kSwayMaxRadians);
    offset.x -= lag_yaw * kSwayMeters;
    offset.y -= lag_pitch * kSwayMeters;
    yaw_kick += lag_yaw * kSwayRotate;
    pitch_kick += lag_pitch * kSwayRotate;

    if (melee) {
        // No muzzle to rise: the impulse becomes a slash across the view,
        // and the spring's overshoot brings the blade back the other way.
        yaw_kick -= state.recoil * kSwingYawRadians;
        roll_kick -= state.recoil * kSwingRollRadians;
        offset.z += state.recoil * kSwingLungeMeters;
    } else {
        pitch_kick += state.recoil * kRecoilPitchRadians;
        yaw_kick += state.recoil * kRecoilYawRadians * (state.shots % 2 == 0 ? 1.0f : -1.0f);
        offset.z -= state.recoil * kRecoilBackMeters;
    }

    if (state.reload_remaining > 0.0f && state.reload_seconds > 0.0f) {
        // sin() over the clip: nothing at either end, everything in the
        // middle, so the weapon leaves and returns to the aim smoothly.
        const float t = 1.0f - state.reload_remaining / state.reload_seconds;
        const float arc = std::sin(t * std::numbers::pi_v<float>);
        offset.y -= arc * kReloadDropMeters;
        pitch_kick -= arc * kReloadPitchRadians;
        roll_kick += arc * kReloadRollRadians;
    }

    if (state.raise_remaining > 0.0f && state.raise_seconds > 0.0f) {
        const float remaining = state.raise_remaining / state.raise_seconds;
        const float drop = remaining * remaining;
        offset.y -= drop * kRaiseDropMeters;
        pitch_kick -= drop * kRaisePitchRadians;
    }

    glm::mat4 model{1.0f};
    model[0] = glm::vec4{right, 0.0f};
    model[1] = glm::vec4{up, 0.0f};
    model[2] = glm::vec4{-forward, 0.0f};
    model[3] =
        glm::vec4{camera.position + right * offset.x + up * offset.y + forward * offset.z, 1.0f};
    model = glm::rotate(model, pitch_kick, glm::vec3{1.0f, 0.0f, 0.0f});
    model = glm::rotate(model, yaw_kick, glm::vec3{0.0f, 1.0f, 0.0f});
    model = glm::rotate(model, roll_kick, glm::vec3{0.0f, 0.0f, 1.0f});
    return model;
}

void draw_capsule(eng::DebugDraw& draw, const glm::vec3& feet, float radius, float height,
                  const glm::vec3& color) {
    constexpr int kSegments = 16;
    const glm::vec3 bottom = feet + glm::vec3{0.0f, radius, 0.0f};
    const glm::vec3 top = feet + glm::vec3{0.0f, height - radius, 0.0f};
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = static_cast<float>(i) / kSegments * 2.0f * std::numbers::pi_v<float>;
        const float a1 = static_cast<float>(i + 1) / kSegments * 2.0f * std::numbers::pi_v<float>;
        const glm::vec3 r0{std::cos(a0) * radius, 0.0f, std::sin(a0) * radius};
        const glm::vec3 r1{std::cos(a1) * radius, 0.0f, std::sin(a1) * radius};
        draw.line(bottom + r0, bottom + r1, color);
        draw.line(top + r0, top + r1, color);
        if (i % 4 == 0) {
            draw.line(bottom + r0, top + r0, color);
        }
    }
    draw.line(feet, feet + glm::vec3{0.0f, height, 0.0f}, color);
}

// Shootable practice dummy. Rendered as a stretched cube, hit-tested as a
// capsule (same math the networked players will use in Milestone 8).
struct Target {
    glm::vec3 home{0.0f};
    glm::vec3 position{0.0f};
    game::Health health;
    float respawn_remaining = 0.0f;  // > 0 while dead
    float patrol_radius = 0.0f;      // 0 = static
    float patrol_phase = 0.0f;

    bool alive() const { return respawn_remaining <= 0.0f; }
};

struct Tracer {
    glm::vec3 from;
    glm::vec3 to;
    float ttl;
};

// Killer and victim are kept as ids as well as names so the feed can show who
// each one is in the colour they are wearing out in the arena.
struct KillFeedEntry {
    std::uint8_t killer;
    std::uint8_t victim;
    std::string killer_name;
    std::string victim_name;
    float ttl;
    bool headshot = false;
};

// Draws a player's identity colour as a chip in front of their name, and
// leaves the cursor on the same line for it.
//
// A chip rather than colouring the name itself: half the palette is dark on
// purpose (see player_color.h), and dark text over a transparent HUD is
// exactly what a name must never be. The chip shows the raw albedo without the
// scene's tonemap -- at this size it needs the full strength, and the hue is
// what carries the match to the figure anyway.
void player_chip(std::uint8_t id, float alpha) {
    const glm::vec3 color = game::player_color(id);
    const float size = ImGui::GetTextLineHeight();
    const ImVec2 top_left = ImGui::GetCursorScreenPos();
    const ImVec2 bottom_right{top_left.x + size, top_left.y + size};
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(top_left, bottom_right,
                        ImGui::GetColorU32(ImVec4{color.r, color.g, color.b, alpha}), 2.0f);
    // Outlined both ways: a dark chip needs an edge against the scoreboard's
    // dark panel, a bright one needs one against the sky behind the kill feed.
    list->AddRect(top_left, bottom_right, ImGui::GetColorU32(ImVec4{0.0f, 0.0f, 0.0f, alpha}),
                  2.0f);
    ImGui::Dummy({size, size});
    ImGui::SameLine();
}

// User preferences persisted next to the executable's working directory.
struct Settings {
    float sensitivity = 0.002f;  // radians per pixel
    float fov_degrees = 70.0f;
    float volume = 1.0f;
    std::string name = "player";
    std::string last_ip = "127.0.0.1";
};

constexpr const char* kSettingsFile = "fps_settings.cfg";

Settings load_settings() {
    Settings s;
    const auto text = eng::read_text_file(kSettingsFile, /*required=*/false);
    if (!text) {
        return s;  // first run
    }
    std::string_view rest = *text;
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = (nl == std::string_view::npos) ? std::string_view{} : rest.substr(nl + 1);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        const auto to_float = [&](float& out) {
            std::from_chars(value.data(), value.data() + value.size(), out);
        };
        if (key == "sensitivity") {
            to_float(s.sensitivity);
        } else if (key == "fov") {
            to_float(s.fov_degrees);
        } else if (key == "volume") {
            to_float(s.volume);
        } else if (key == "name" && !value.empty() && value.size() <= game::kMaxNameLength) {
            s.name = std::string(value);
        } else if (key == "last_ip" && !value.empty() && value.size() < 64) {
            s.last_ip = std::string(value);
        }
    }
    s.sensitivity = std::clamp(s.sensitivity, 0.0002f, 0.02f);
    s.fov_degrees = std::clamp(s.fov_degrees, 50.0f, 120.0f);
    s.volume = std::clamp(s.volume, 0.0f, 1.0f);
    return s;
}

void save_settings(const Settings& s) {
    std::string out = std::format("sensitivity={}\nfov={}\nvolume={}\nname={}\nlast_ip={}\n",
                                  s.sensitivity, s.fov_degrees, s.volume, s.name, s.last_ip);
    std::ofstream file(kSettingsFile, std::ios::binary | std::ios::trunc);
    if (file) {
        file << out;
    } else {
        eng::log::warn("Could not save settings to {}", kSettingsFile);
    }
}

enum class Mode : std::uint8_t {
    Menu,     // main menu over an orbiting arena view
    Offline,  // practice range (targets)
    Online,   // connected to a server
    Replay,   // watching a recorded match, no local player
};

// Loads one weapon's boxes from assets/weapons/<name>.glb.
//
// `name` is a literal from the arsenal list rather than the parsed config's
// `name=` field: a value out of an asset file has no business being joined
// onto a path, which is the same reason fire_sound is validated in
// parse_weapon_config.
//
// A weapon that fails to load simply is not drawn. A GUN that loads without a
// `muzzle` node is a broken asset and says so here, because the alternative
// is a gun that silently fires blanks for the rest of the build's life.
WeaponModel load_weapon_model(eng::AssetCache& assets, const char* name, bool melee) {
    WeaponModel model;
    const eng::GltfModel* gltf = assets.model("weapons/" + std::string(name) + ".glb");
    if (gltf == nullptr) {
        return model;
    }

    // One GPU copy per glTF primitive, shared by every box that uses it:
    // gen_weapons.py emits one mesh per material over a single unit cube, so
    // a twelve-box sniper uploads four meshes.
    std::vector<int> first_primitive(gltf->meshes.size(), 0);
    for (std::size_t i = 0; i < gltf->meshes.size(); ++i) {
        first_primitive[i] = static_cast<int>(model.primitives.size());
        for (const eng::GltfPrimitive& primitive : gltf->meshes[i].primitives) {
            RenderPrimitive rp{eng::GpuMesh::upload(primitive.mesh), glm::vec3{1.0f}, -1};
            if (primitive.material >= 0) {
                rp.color = glm::vec3(
                    gltf->materials[static_cast<std::size_t>(primitive.material)].base_color);
            }
            model.primitives.push_back(std::move(rp));
        }
    }

    eng::Bounds extent;
    for (const eng::GltfNode& node : gltf->nodes) {
        if (node.mesh < 0) {
            // Mesh-less nodes are markers, read by name -- the same mechanism
            // the maps use for `spawn_N`.
            if (node.name == "muzzle") {
                model.muzzle = glm::vec3(node.transform[3]);
            }
            continue;
        }
        const std::size_t mesh = static_cast<std::size_t>(node.mesh);
        for (std::size_t i = 0; i < gltf->meshes[mesh].primitives.size(); ++i) {
            eng::Bounds local;
            for (const eng::Vertex& vertex : gltf->meshes[mesh].primitives[i].mesh.vertices) {
                local.expand(vertex.position);
            }
            extent.expand(local, node.transform);
            model.boxes.push_back({node.transform, first_primitive[mesh] + static_cast<int>(i)});
        }
    }
    // -Z is forward, so the furthest-forward geometry is the smallest z.
    if (!extent.empty()) {
        model.reach = std::max(0.0f, -extent.min.z);
    }

    if (!melee && !model.muzzle) {
        eng::log::error("Weapon '{}' has no `muzzle` node; it will fire without a flash", name);
    }
    eng::log::info("Weapon {}: {} boxes, {} materials, reach {:.3f} m", name, model.boxes.size(),
                   model.primitives.size(), model.reach);
    return model;
}

game::InputCommand make_command(const eng::InputState& input, float yaw, float pitch,
                                std::uint32_t sequence, std::uint8_t weapon_slot) {
    game::InputCommand command;
    command.sequence = sequence;
    command.yaw = yaw;
    command.pitch = pitch;
    command.weapon_slot = weapon_slot;
    game::set_button(command, game::Button::Forward, input.is_down(eng::Key::W));
    game::set_button(command, game::Button::Back, input.is_down(eng::Key::S));
    game::set_button(command, game::Button::Left, input.is_down(eng::Key::A));
    game::set_button(command, game::Button::Right, input.is_down(eng::Key::D));
    game::set_button(command, game::Button::Jump, input.is_down(eng::Key::Space));
    game::set_button(command, game::Button::Fire, input.is_down(eng::MouseButton::Left));
    game::set_button(command, game::Button::Reload, input.is_down(eng::Key::R));
    game::set_button(command, game::Button::Sprint, input.is_down(eng::Key::LeftShift));
    game::set_button(command, game::Button::Crouch, input.is_down(eng::Key::LeftControl));
    return command;
}

// Screen-space feedback for a landed shot: a brief crosshair flash.
struct Hitmarker {
    float ttl = 0.0f;
    bool kill = false;
    bool headshot = false;
};

// Damage dealt, floated in world space above where it landed.
struct DamageNumber {
    glm::vec3 world;
    float amount = 0.0f;
    float ttl = 0.0f;
};

}  // namespace

int main(int argc, char** argv) {
    const ClientArgs args = parse_args(argc, argv);
    // Nothing else in the tree logs at Trace, so --audio-trace is in practice
    // an audio-only dump: one line per spatialized sound, giving the emitter,
    // the listener and the emitter's coordinates in the listener's own frame.
    // Far too noisy to leave on, and the only way to check the left/right
    // sign of a running client from the outside.
    eng::log::set_level(args.audio_trace ? eng::log::Level::Trace : eng::log::Level::Debug);
    eng::log::info("FPS client starting (engine v{})", eng::version_string());

    auto window =
        eng::Window::create({.title = "FPS", .width = 1280, .height = 720, .vsync = args.vsync});
    if (!window) {
        return 1;
    }
    auto imgui = eng::ImGuiLayer::create(*window);
    // Both vertex stages get the same skinning helper prepended, so the two
    // passes cannot disagree about where a skinned vertex ends up.
    const std::string lit_vertex = std::string(kSkinningGlsl) + std::string(kLitVertexSource);
    const std::string depth_vertex = std::string(kSkinningGlsl) + std::string(kDepthVertexSource);
    auto lit_shader = eng::Shader::create("lit", lit_vertex, kLitFragmentSource);
    auto depth_shader = eng::Shader::create("shadow_depth", depth_vertex, kDepthFragmentSource);
    auto debug_draw = eng::DebugDraw::create();
    auto shadow_map = eng::ShadowMap::create(kShadowResolution);
    auto particle_renderer = eng::ParticleRenderer::create(kMaxParticles);
    auto postfx = eng::PostFx::create(window->width_px(), window->height_px());
    auto sky = eng::Sky::create();
    auto joint_texture = eng::JointTexture::create(kMaxJoints);
    if (!imgui || !lit_shader || !depth_shader || !debug_draw || !shadow_map ||
        !particle_renderer || !postfx || !sky || !joint_texture) {
        return 1;
    }
    eng::ParticlePool particles{kMaxParticles};
    eng::PostFx::Settings postfx_settings;
    eng::Sky::Params sky_params;

    // --- assets & scene ---------------------------------------------------
    const auto assets_root = eng::find_assets_root();
    if (!assets_root) {
        eng::log::error("Could not locate the assets/ directory");
        return 1;
    }
    eng::AssetCache assets{*assets_root};
    const std::string map_path = eng::normalize_asset_path(args.map);
    if (map_path.empty() || eng::asset_path_escapes_root(map_path)) {
        eng::log::error("--map '{}' is not a path inside assets/", args.map);
        return 1;
    }
    const eng::GltfModel* arena = assets.model(map_path);
    if (arena == nullptr) {
        eng::log::error("Could not load map '{}'", map_path);
        return 1;
    }
    eng::log::info("Map: {}", map_path);

    // Base color images are sRGB-encoded; the shader wants linear values, so
    // GL does the conversion on sample. No vertical flip: glTF's UV origin is
    // top-left and stb decodes top-row-first, so the two conventions cancel.
    // The list stays index-aligned with arena->images so material indices
    // work directly; anything that failed to decode gets the missing-texture
    // checkerboard rather than silently rendering untextured.
    //
    // Normal maps go in the same list but are uploaded as plain RGBA8: they
    // encode directions, and running them through the sRGB decode would bend
    // every one of them toward the surface normal.
    std::vector<bool> is_normal_map(arena->images.size(), false);
    for (const eng::GltfMaterial& material : arena->materials) {
        if (material.normal_image >= 0 &&
            static_cast<std::size_t>(material.normal_image) < is_normal_map.size()) {
            is_normal_map[static_cast<std::size_t>(material.normal_image)] = true;
        }
    }
    std::vector<eng::Texture2D> arena_textures;
    arena_textures.reserve(arena->images.size());
    for (std::size_t i = 0; i < arena->images.size(); ++i) {
        const eng::GltfImage& image = arena->images[i];
        if (image.valid()) {
            arena_textures.push_back(eng::Texture2D::from_pixels(image.width, image.height,
                                                                 image.pixels, !is_normal_map[i]));
        } else {
            eng::log::warn("Arena image '{}' has no pixels; using the missing-texture pattern",
                           image.name);
            arena_textures.push_back(
                eng::Texture2D::checkerboard(64, 8, {255, 0, 255}, {40, 40, 40}));
        }
    }

    std::vector<std::vector<RenderPrimitive>> render_meshes;
    render_meshes.reserve(arena->meshes.size());
    for (const eng::GltfMesh& mesh : arena->meshes) {
        std::vector<RenderPrimitive> primitives;
        for (const eng::GltfPrimitive& primitive : mesh.primitives) {
            RenderPrimitive rp{eng::GpuMesh::upload(primitive.mesh), glm::vec3{1.0f}, -1, -1, 1.0f};
            if (primitive.material >= 0) {
                const eng::GltfMaterial& material =
                    arena->materials[static_cast<std::size_t>(primitive.material)];
                rp.color = glm::vec3(material.base_color);
                if (material.base_color_image >= 0 &&
                    static_cast<std::size_t>(material.base_color_image) < arena_textures.size()) {
                    rp.texture = material.base_color_image;
                }
                if (material.normal_image >= 0 &&
                    static_cast<std::size_t>(material.normal_image) < arena_textures.size()) {
                    rp.normal_texture = material.normal_image;
                    rp.normal_scale = material.normal_scale;
                }
            }
            primitives.push_back(std::move(rp));
        }
        render_meshes.push_back(std::move(primitives));
    }

    // Stand-ins for untextured geometry (players, targets) so the lit shader
    // always has something bound to sample. (128,128,255) decodes to a
    // straight-up tangent-space normal, so a material with no normal map
    // shades exactly as it did before this existed -- belt and braces
    // alongside the shader's zero-tangent fallback, since sampling a unit
    // with nothing bound is undefined rather than harmless. It is uploaded
    // LINEAR: through the sRGB decode 128 would land at 0.21, not 0.5, and
    // every "flat" surface would lean 35 degrees.
    const eng::Texture2D white =
        eng::Texture2D::checkerboard(4, 1, {255, 255, 255}, {255, 255, 255});
    constexpr std::array<std::uint8_t, 4> kFlatNormalTexel{128, 128, 255, 255};
    const eng::Texture2D flat_normal =
        eng::Texture2D::from_pixels(1, 1, kFlatNormalTexel, /*srgb=*/false);
    const eng::GpuMesh cube = eng::GpuMesh::upload(eng::MeshData::unit_cube());

    // --- skinned character -------------------------------------------------
    // One mesh, one skeleton, shared by every figure on screen: only the
    // joint matrices differ per figure.
    const eng::GltfModel* character_model = assets.model("models/character.glb");
    if (character_model == nullptr || character_model->skins.empty() ||
        character_model->meshes.empty()) {
        eng::log::error("Character model missing or has no skin");
        return 1;
    }
    auto character_skeleton = eng::Skeleton::from_gltf(*character_model, character_model->skins[0]);
    if (!character_skeleton) {
        return 1;
    }
    if (character_skeleton->joint_count() > kMaxJoints) {
        eng::log::error("Character has {} joints; the shader holds {}",
                        character_skeleton->joint_count(), kMaxJoints);
        return 1;
    }
    const std::vector<eng::AnimationClip> character_clips =
        eng::build_clips(*character_model, *character_skeleton);
    const eng::AnimationClip* clip_idle = eng::find_clip(character_clips, "idle");
    const eng::AnimationClip* clip_run = eng::find_clip(character_clips, "run");
    const eng::AnimationClip* clip_jump = eng::find_clip(character_clips, "jump");
    if (clip_idle == nullptr || clip_run == nullptr || clip_jump == nullptr) {
        eng::log::error("Character is missing one of the idle/run/jump clips");
        return 1;
    }
    const eng::GpuMesh character_mesh =
        eng::GpuMesh::upload(character_model->meshes[0].primitives[0].mesh);
    eng::log::info("Character: {} joints, {} clips", character_skeleton->joint_count(),
                   character_clips.size());

    // Resolves a CharacterAnimation into joint matrices appended to `pool`,
    // returning the offset. Scratch poses are function-local statics so the
    // per-frame path does not allocate.
    const auto append_character_pose = [&](const CharacterAnimation& animation,
                                           std::vector<glm::mat4>& pool) -> int {
        static eng::Pose idle_pose;
        static eng::Pose run_pose;
        static eng::Pose blended;
        static std::vector<glm::mat4> matrices;

        eng::sample_clip(*character_skeleton, *clip_idle, animation.clip_time, true, idle_pose);
        if (animation.airborne) {
            // Jump does not loop: it holds its last frame while in the air.
            eng::sample_clip(*character_skeleton, *clip_jump, animation.air_time, false, run_pose);
            eng::blend_poses(idle_pose, run_pose, 1.0f, blended);
        } else {
            eng::sample_clip(*character_skeleton, *clip_run, animation.clip_time, true, run_pose);
            eng::blend_poses(idle_pose, run_pose, animation.run_weight, blended);
        }
        eng::pose_to_joint_matrices(*character_skeleton, blended, matrices);

        const auto offset = static_cast<int>(pool.size());
        pool.insert(pool.end(), matrices.begin(), matrices.end());
        return offset;
    };

    // Rebuilt every frame but kept allocated across frames.
    std::vector<DrawItem> draw_items;
    std::vector<glm::mat4> joint_pool;

    // Offline practice has no remote players, so a mannequin stands near the
    // centre platform: without it there is nothing skinned to look at, and
    // the whole feature would only be visible in a networked session.
    CharacterAnimation dummy_animation;
    const glm::vec3 dummy_position{11.0f, 0.0f, 10.0f};
    // Animation phase has to persist per player across frames, or every
    // figure resets to the start of its cycle every frame and never moves.
    std::unordered_map<std::uint8_t, CharacterAnimation> remote_animations;

    eng::Scene scene;
    std::vector<glm::vec3> spawn_points;
    for (const eng::GltfNode& node : arena->nodes) {
        const eng::EntityId id = scene.create(node.name);
        eng::Entity* entity = scene.get(id);
        entity->transform = eng::Transform::from_matrix(node.transform);
        entity->mesh = node.mesh;
        if (node.name.starts_with("spawn_")) {
            spawn_points.push_back(entity->transform.position);
        }
    }

    // The sun's shadow projection is fitted to the static geometry once:
    // the arena never moves, and players are always inside it. Raised a
    // little so a player standing on the tallest platform still casts.
    eng::Bounds scene_bounds;
    for (const eng::GltfNode& node : arena->nodes) {
        if (node.mesh < 0) {
            continue;
        }
        for (const eng::GltfPrimitive& primitive :
             arena->meshes[static_cast<std::size_t>(node.mesh)].primitives) {
            eng::Bounds local;
            for (const eng::Vertex& vertex : primitive.mesh.vertices) {
                local.expand(vertex.position);
            }
            scene_bounds.expand(local, node.transform);
        }
    }
    scene_bounds.max.y += 2.5f;
    const glm::mat4 light_view_projection =
        eng::directional_light_view_projection(kSunDirection, scene_bounds);

    // --- physics ------------------------------------------------------------
    eng::PhysicsWorld world;
    for (const eng::GltfNode& node : arena->nodes) {
        if (node.mesh < 0) {
            continue;
        }
        for (const eng::GltfPrimitive& primitive :
             arena->meshes[static_cast<std::size_t>(node.mesh)].primitives) {
            world.add_static_mesh(primitive.mesh, node.transform);
        }
    }
    world.optimize();
    eng::log::info("Physics: {} static bodies", world.body_count());

    const glm::vec3 spawn = spawn_points.empty() ? glm::vec3{0.0f, 1.0f, 0.0f} : spawn_points[0];
    eng::CharacterController controller{world, spawn};
    game::PlayerState player;
    player.position = spawn;
    game::PlayerState previous_player = player;

    // --- gameplay: weapon, targets, audio ---------------------------------
    auto audio = eng::AudioEngine::create();  // optional: game runs silent if it fails
    // Two deliberately separate paths. sound() is head-relative: it is for
    // things that happen TO this player -- their own weapon, their own death,
    // the hit confirmation -- which have no location in the arena, and which
    // panning would place somewhere outside their own head. sound_at() is for
    // things that happen ELSEWHERE, where knowing the direction is the whole
    // value of hearing it at all.
    const auto sound = [&](const char* name, float volume = 1.0f, float pitch = 1.0f) {
        if (audio) {
            audio->play(*assets_root / "sounds" / name, volume, pitch);
        }
    };
    const auto sound_at = [&](const char* name, const glm::vec3& position, float volume = 1.0f,
                              float pitch = 1.0f) {
        if (audio) {
            audio->play_at(*assets_root / "sounds" / name, position, volume, pitch);
        }
    };

    // Footsteps. The one sound in the game that is pure information: an enemy
    // crossing the arena in silence is a fight you never knew you were in.
    // Everyone else's steps are positional and are the point of the feature;
    // your own are 2D and quiet, because they tell you nothing you did not
    // already know and at full weight would drown out the ones that do.
    constexpr std::array<const char*, game::kFootstepVariants> kStepSounds{
        "step1.wav", "step2.wav", "step3.wav", "step4.wav"};
    constexpr float kSelfStepVolume = 0.45f;
    game::FootstepState self_footsteps;
    std::unordered_map<std::uint8_t, game::FootstepState> remote_footsteps;

    // Slot order must match the server's: 1=rifle, 2=smg, 3=shotgun, 4=sniper.
    game::Arsenal arsenal;
    // Index-aligned with arsenal.weapons, which is why the model is loaded in
    // the same step as the config rather than in a second loop: a weapon whose
    // .cfg fails to parse takes no slot, and a parallel list built separately
    // would hand the viewmodel the wrong gun from there on.
    std::vector<WeaponModel> weapon_models;
    for (const char* weapon_name : {"rifle", "smg", "shotgun", "sniper", "knife"}) {
        const auto text =
            eng::read_text_file(*assets_root / "weapons" / (std::string(weapon_name) + ".cfg"));
        if (!text) {
            continue;
        }
        if (const auto parsed = game::parse_weapon_config(*text)) {
            arsenal.weapons.push_back(*parsed);
            weapon_models.push_back(load_weapon_model(assets, weapon_name, parsed->melee));
        }
    }
    if (arsenal.empty()) {
        arsenal.weapons.push_back(game::WeaponConfig{});
    }
    // The weapon whose model to draw, or nullptr when the slot has none (a
    // failed load, or the default config the line above falls back to).
    const auto weapon_model_for = [&weapon_models](std::uint8_t slot) -> const WeaponModel* {
        if (slot >= weapon_models.size() || !weapon_models[slot].loaded()) {
            return nullptr;
        }
        return &weapon_models[slot];
    };
    game::Loadout loadout;
    game::reset_loadout(loadout, arsenal);
    // The slot is state rather than an edge (it rides every command), so
    // seeding it here is all --weapon needs to work in BOTH offline and
    // online runs. There is no other way to put a chosen gun in front of an
    // automated screenshot: the keys 1-5 need a keyboard.
    std::uint8_t desired_slot = args.weapon ? arsenal.clamp_slot(*args.weapon) : 0;

    // The bang for a weapon slot, straight out of its config. Deliberately not
    // arsenal.at(), whose out-of-range clamp to slot 0 would answer "what does
    // slot 9 sound like?" with the rifle -- a confident wrong answer about a
    // weapon this build does not have. The generic bang is the honest one.
    const auto fire_sound_for = [&arsenal](std::uint8_t slot) -> const char* {
        return slot < arsenal.size() ? arsenal.weapons[slot].fire_sound.c_str() : "fire.wav";
    };

    constexpr float kTargetRadius = 0.4f;
    constexpr float kTargetHeight = 1.8f;
    constexpr float kTargetRespawnSeconds = 3.0f;
    std::vector<Target> targets = {
        {.home = {8.0f, 0.0f, -14.0f}, .position = {}, .health = {50.0f, 50.0f}},
        {.home = {-8.0f, 0.0f, -14.0f}, .position = {}, .health = {50.0f, 50.0f}},
        {.home = {0.0f, 1.5f, 0.0f}, .position = {}, .health = {50.0f, 50.0f}},  // on platform
        {.home = {12.0f, 0.0f, 6.0f},
         .position = {},
         .health = {50.0f, 50.0f},
         .respawn_remaining = 0.0f,
         .patrol_radius = 4.0f},
        {.home = {-12.0f, 0.0f, 6.0f},
         .position = {},
         .health = {50.0f, 50.0f},
         .respawn_remaining = 0.0f,
         .patrol_radius = 4.0f,
         .patrol_phase = 3.14f},
    };
    for (Target& target : targets) {
        target.position = target.home;
    }
    int kills = 0;
    int deaths = 0;
    std::vector<Tracer> tracers;
    double sim_time = 0.0;

    // --- settings, menu state, networking ---------------------------------
    Settings settings = load_settings();
    if (args.name != "player") {
        settings.name = args.name;  // CLI overrides the saved name
    }
    if (audio) {
        audio->set_master_volume(settings.volume);
    }

    // Declared up here because the replay branch below aims it at the first
    // recorded spawn.
    game::FlyCamera fly;
    std::optional<game::NetClient> net;
    std::optional<game::Prediction> prediction;
    std::optional<ReplayPlayback> playback;
    std::unordered_map<std::uint8_t, CharacterAnimation> replay_animations;
    Mode mode = Mode::Menu;
    std::string menu_error;

    // A ServerReject(VersionMismatch) is not a dead end, it is a race. Client
    // and server ship separately and there is no in-protocol compatibility,
    // so a protocol bump has a window -- minutes, while the other half
    // deploys -- in which one of them is ahead. Dropping the player into the
    // menu with "server rejected the connection" during that window is
    // accurate and useless: nothing they can do from there helps, and the
    // thing that WILL fix it is already happening. So wait it out and say so.
    // docs/deploy.md, "a protocol bump is a deliberate two-step".
    struct Reconnect {
        std::string host;  // the address to go back to; outlives the waiting
        int attempts = 0;
        float seconds_until = 0.0f;
        bool waiting() const { return attempts > 0; }
        void stop_waiting() {
            attempts = 0;
            seconds_until = 0.0f;
        }
    };
    Reconnect reconnect;
    // Linear, capped. The window is bounded by a CI run, so backing off past
    // a few seconds only adds dead time to the recovery it is waiting for.
    const auto retry_delay = [](int attempts) {
        return std::min(15.0f, 1.5f * static_cast<float>(attempts));
    };
    char menu_name[17]{};
    char menu_ip[64]{};
    std::snprintf(menu_name, sizeof(menu_name), "%s", settings.name.c_str());
    std::snprintf(menu_ip, sizeof(menu_ip), "%s", settings.last_ip.c_str());
#if defined(__EMSCRIPTEN__)
    // Browsers connect over WebSockets; default to a ws:// URL. A deployed
    // build bakes in its server via -DFPS_WEB_SERVER_URL=wss://host (see
    // docs/deploy.md); otherwise default to a local dev server. Settings are
    // not persisted in the browser, so last_ip is always the native default.
    if (std::string_view(menu_ip).find("://") == std::string_view::npos) {
#if defined(FPS_WEB_SERVER_URL)
        std::snprintf(menu_ip, sizeof(menu_ip), "%s", FPS_WEB_SERVER_URL);
#else
        std::snprintf(menu_ip, sizeof(menu_ip), "ws://localhost:7778");
#endif
    }
#endif

    if (args.connect_host) {
        net = game::NetClient::connect(*args.connect_host, args.port, settings.name);
        if (!net) {
            eng::log::error("Failed to start network client");
            return 1;
        }
        reconnect.host = *args.connect_host;
        // The client predicts on its OWN physics world with the shared
        // movement code; the server remains authoritative.
        prediction.emplace(world, spawn);
        mode = Mode::Online;
        if (args.net_sim.enabled()) {
            eng::log::warn("Network simulation active: {} ms +{} ms jitter, {:.1f}% loss",
                           args.net_sim.latency_ms, args.net_sim.jitter_ms,
                           args.net_sim.loss_percent);
        }
    } else if (args.replay_path) {
        auto loaded = game::read_replay_file(*args.replay_path);
        if (!loaded) {
            return 1;
        }
        playback.emplace();
        playback->replay = std::move(*loaded);
        reset_playback(*playback, world);
        mode = Mode::Replay;
        // Park the free camera looking at the first recorded spawn. The
        // default fly position is arbitrary, and a replay that opens facing a
        // wall reads as broken.
        if (!playback->replay.players.empty()) {
            // Default to chasing the first recorded player. Watching someone
            // is what a replay is for; the free camera is one click away and
            // framing an empty arena well is guesswork.
            playback->follow = playback->replay.players[0].id;

            const glm::vec3 target = playback->replay.players[0].spawn;
            // Behind the spawn relative to the arena centre and raised, so
            // the opening shot looks inward across the map rather than over
            // a wall.
            const glm::vec3 outward =
                glm::length(target) > 0.1f ? glm::normalize(target) : glm::vec3{0.0f, 0.0f, 1.0f};
            fly.camera.position = target + outward * 4.0f + glm::vec3{0.0f, 6.0f, 0.0f};
            // Aimed at the middle of the map rather than at the spawn itself,
            // so the opening shot frames the arena the player will move into.
            const glm::vec3 to_target =
                glm::normalize(glm::vec3{0.0f, 1.0f, 0.0f} - fly.camera.position);
            fly.camera.yaw = std::atan2(to_target.x, -to_target.z);
            fly.camera.pitch = std::asin(to_target.y);
        }
        eng::log::info("Replay: '{}', {} players, {} frames", args.replay_path.value(),
                       playback->replay.players.size(), playback->replay.frames.size());
    } else if (args.run_seconds) {
        mode = Mode::Offline;  // automated runs go straight to the range
    }
    bool online = mode == Mode::Online;
    std::deque<game::InputCommand> recent_commands;  // newest first
    std::uint32_t client_tick = 0;
    double remote_render_tick = -1.0;  // fractional server tick remote players render at
    std::array<float, 240> prediction_error_history{};
    std::size_t prediction_error_cursor = 0;
    float last_reconcile_error = 0.0f;
    eng::NetSimConfig sim_config = args.net_sim;
    std::deque<KillFeedEntry> kill_feed;
    Hitmarker hitmarker;
    // Where each player was last hit, so a death can be reported as a headshot.
    std::array<game::HitZone, game::kMaxPlayers> last_damage_zone{};
    std::vector<DamageNumber> damage_numbers;
    const auto player_name = [&](std::uint8_t id) -> std::string {
        if (!online) {
            return "?";
        }
        const auto it = net->players().find(id);
        return it != net->players().end() ? it->second.name : "world";
    };

    float view_yaw = 0.0f;
    float view_pitch = 0.0f;
    std::uint32_t input_sequence = 0;

    ViewmodelState viewmodel;
    // The flash owed to this frame's own shots. Deferred rather than emitted
    // where the shot is taken, because a shot is taken inside the fixed tick
    // and the barrel it leaves is not placed until the camera exists further
    // down the frame. `armed` also covers several shots in one frame (an smg
    // at 900 rpm can tick twice): one flash per frame is all that would be
    // visible anyway.
    bool muzzle_flash_armed = false;
    std::uint32_t muzzle_flash_seed = 0;

    float smoothed_eye_height = game::kMove.eye_height;
    // Killcam playback position, in seconds. Reset while alive rather than on
    // arrival: the message can land a frame before or after PlayerDied, and
    // keying off "am I dead" makes the ordering of the two irrelevant.
    float kill_cam_elapsed = 0.0f;
    fly.camera.position = spawn + glm::vec3{0.0f, 8.0f, 12.0f};
    fly.camera.pitch = -0.5f;
    bool fly_mode = false;
    bool draw_physics = true;

    eng::InputState input;
    eng::Clock clock;
    eng::FixedTimestep step{1.0 / game::kTickRate};

    window->set_relative_mouse(mode != Mode::Menu);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
#if !defined(__EMSCRIPTEN__)
    // WebGL's default framebuffer is already sRGB-correct; there is no
    // GL_FRAMEBUFFER_SRGB enable in GLES 3.0.
    glEnable(GL_FRAMEBUFFER_SRGB);
#endif

    std::array<float, 240> frame_ms_history{};
    std::size_t frame_ms_cursor = 0;

#if defined(__EMSCRIPTEN__)
    int web_frames = 0;
    int web_distinct_colors = 0;
    float web_mean_luma = 0.0f;
#endif

    bool running = true;
    // The frame body is a lambda so the same code drives a native while-loop
    // and the browser's requestAnimationFrame callback (emscripten owns the
    // loop there and code must not block).
    const auto frame = [&]() {
        if (!window->poll(input, &*imgui)) {
            running = false;
        }
        if (input.was_pressed(eng::Key::Escape) && mode != Mode::Menu) {
            window->set_relative_mouse(!window->relative_mouse());
        }
        if (input.was_pressed(eng::Key::F1)) {
            fly_mode = !fly_mode;
            eng::log::debug("fly mode: {}", fly_mode);
        }
        if (input.was_pressed(eng::Key::F3)) {
            draw_physics = !draw_physics;
        }
        // Weapon selection is state, not an edge: we keep sending the chosen
        // slot every tick so a dropped packet cannot lose the switch.
        if (mode != Mode::Menu) {
            const std::array<eng::Key, 5> slot_keys = {
                eng::Key::Num1, eng::Key::Num2, eng::Key::Num3, eng::Key::Num4, eng::Key::Num5};
            for (std::uint8_t i = 0; i < slot_keys.size(); ++i) {
                if (input.was_pressed(slot_keys[i]) && i < arsenal.size()) {
                    desired_slot = i;
                }
            }
        }

        const double dt = clock.tick();

        // View angles update every render frame for minimal aim latency;
        // simulation consumes the latest angles at each fixed tick.
        if (window->relative_mouse() && !fly_mode && mode != Mode::Menu) {
            view_yaw += input.mouse_dx() * settings.sensitivity;
            view_pitch -= input.mouse_dy() * settings.sensitivity;
            view_pitch = std::clamp(view_pitch, -eng::Camera::kMaxPitchRadians,
                                    eng::Camera::kMaxPitchRadians);
        }

        const bool reload_requested = input.was_pressed(eng::Key::R);
        bool reload_consumed = false;

        if (online) {
            net->set_simulation(sim_config);
            net->poll();
            if (net->self_alive()) {
                kill_cam_elapsed = 0.0f;
            }
            // Geometry was loaded before the server could be asked which map
            // it runs, so the welcome is the first chance to find out. A
            // mismatch is not a degraded match, it is a different world:
            // shots stop at walls the server has never heard of and every
            // authoritative position is meaningless. Refuse it, and say what
            // to relaunch with rather than leaving the player to guess why
            // the game is behaving impossibly.
            const bool wrong_map = net->state() == game::NetClient::State::InGame &&
                                   !net->server_map().empty() && net->server_map() != map_path;
            // Made it in: whatever we were waiting out is over.
            if (net->state() == game::NetClient::State::InGame && reconnect.waiting()) {
                eng::log::info("Reconnected after {} attempt(s)", reconnect.attempts);
                reconnect.stop_waiting();
            }
            const bool version_mismatch =
                net->state() == game::NetClient::State::Rejected &&
                net->reject_reason() == game::RejectReason::VersionMismatch;
            // Server gone or refused us: back to the menu.
            if (wrong_map || net->state() == game::NetClient::State::Disconnected ||
                net->state() == game::NetClient::State::Rejected) {
                if (wrong_map) {
                    reconnect.stop_waiting();
                    eng::log::error("Server runs '{}' but this client loaded '{}'",
                                    net->server_map(), map_path);
                    menu_error = "server is on " + net->server_map() + "; relaunch with --map " +
                                 net->server_map();
                } else if (net->state() == game::NetClient::State::Rejected && !version_mismatch) {
                    // Full, or a bad name. Neither gets better by waiting.
                    reconnect.stop_waiting();
                    menu_error = std::string("server rejected the connection: ") +
                                 game::reject_reason_name(net->reject_reason());
                } else if (version_mismatch || reconnect.waiting()) {
                    // A fresh mismatch, or a connection that dropped while we
                    // were already waiting one out -- a server mid-restart
                    // refuses the socket before it can refuse the hello, and
                    // reporting that as its own error would stack a red
                    // "disconnected" line on top of the countdown explaining
                    // it. Same story, same wait, one attempt further along.
                    menu_error.clear();
                    ++reconnect.attempts;
                    reconnect.seconds_until = retry_delay(reconnect.attempts);
                } else {
                    menu_error = "disconnected from server";
                }
                net.reset();
                prediction.reset();
                online = false;
                mode = Mode::Menu;
                kill_feed.clear();
                recent_commands.clear();
                remote_render_tick = -1.0;
                window->set_relative_mouse(false);
            }
        }

        step.advance(dt);
        while (step.consume_tick()) {
            sim_time += game::kTickSeconds;
            ++client_tick;
            previous_player = player;

            if (mode == Mode::Replay) {
                // One recorded frame per simulation tick at 1x. The debt
                // accumulator lets fractional speeds run smoothly instead of
                // stepping in bursts.
                if (!playback->paused) {
                    playback->frame_debt += playback->speed;
                    while (playback->frame_debt >= 1.0f && !playback->finished()) {
                        playback->frame_debt -= 1.0f;
                        step_playback(*playback, world);
                    }
                }
                continue;  // no local player to simulate
            }

            if (fly_mode || mode == Mode::Menu) {
                continue;
            }

            // Applied to both modes so screenshot runs can aim at whatever
            // the change under test needs to show. Pitch is clamped the same
            // way the mouse path clamps it -- past +-89 degrees the view
            // basis flips over and the frame is useless as evidence.
            if (args.fixed_yaw) {
                view_yaw = *args.fixed_yaw;
            }
            if (args.fixed_pitch) {
                view_pitch = std::clamp(*args.fixed_pitch, -eng::Camera::kMaxPitchRadians,
                                        eng::Camera::kMaxPitchRadians);
            }
            // --auto-fire PULLS the trigger rather than holding it down. Held,
            // it could never fire a semi-automatic weapon at all -- the
            // shotgun and the sniper need the trigger released between shots,
            // so both fired exactly once and then sat there, and nothing that
            // needs a shotgun to keep shooting could be verified. Every other
            // tick costs an automatic weapon at most one tick of delay against
            // its own cooldown.
            const bool auto_fire_now = args.auto_fire && client_tick % 2 == 0;

            if (online) {
                // Predict locally with the shared movement code; the server
                // corrects us via reconcile() below. While dead, inputs keep
                // flowing (so acks stay in sync) but nothing is predicted.
                game::InputCommand command =
                    make_command(input, view_yaw, view_pitch, input_sequence++, desired_slot);
                if (!window->relative_mouse() && !args.auto_fire) {
                    game::set_button(command, game::Button::Fire, false);  // menu clicks
                }
                if (args.auto_fire) {
                    game::set_button(command, game::Button::Fire, auto_fire_now);
                }
                if (args.auto_walk) {
                    game::set_button(command, game::Button::Forward, true);
                }
                if (net->self_alive()) {
                    prediction->tick(command);
                }
                recent_commands.push_front(command);
                if (recent_commands.size() > 8) {
                    recent_commands.pop_back();
                }
                net->send_input(
                    recent_commands, client_tick,
                    remote_render_tick > 0.0 ? static_cast<std::uint32_t>(remote_render_tick) : 0u);
                player = prediction->state();
                previous_player = prediction->previous_state();
                continue;  // offline gameplay (targets/weapon) stays offline
            }

            game::InputCommand command =
                make_command(input, view_yaw, view_pitch, input_sequence++, desired_slot);
            if (args.auto_fire) {
                game::set_button(command, game::Button::Fire, auto_fire_now);
            }
            if (args.auto_walk) {
                game::set_button(command, game::Button::Forward, true);
            }
            const bool was_on_ground = player.on_ground;
            game::advance_player(player, command, game::kTickSeconds, controller, world);
            if (was_on_ground && !player.on_ground && player.velocity.y > 0.0f) {
                sound("jump.wav", 0.5f);
            }

            // Fell out of the world -> count a death and respawn.
            if (player.position.y < -20.0f) {
                player = {};
                player.position = spawn;
                ++deaths;
                sound("death.wav", 0.8f);
            }

            // --- weapon -------------------------------------------------
            // Mouse capture gates firing so menu clicks do not shoot;
            // --auto-fire is an explicit request and bypasses that.
            const bool fire_held = game::has_button(command, game::Button::Fire) &&
                                   (window->relative_mouse() || args.auto_fire);
            const game::WeaponTickResult shot =
                game::update_loadout(loadout, arsenal, desired_slot, fire_held,
                                     reload_requested && !reload_consumed, game::kTickSeconds);
            const game::WeaponConfig& weapon_config = arsenal.at(loadout.slot);
            reload_consumed = true;
            if (shot.reload_started) {
                sound("reload.wav");
            }
            if (shot.dry_fired) {
                sound("dry.wav", 0.7f);
            }
            if (shot.fired) {
                sound(weapon_config.fire_sound.c_str(), 0.9f);
                fire_viewmodel(viewmodel, weapon_config);
                // The flash comes out of the drawn barrel, which is not
                // placed until the camera is built further down the frame.
                muzzle_flash_armed = true;
                muzzle_flash_seed = input_sequence;
                const glm::vec3 eye =
                    player.position + glm::vec3{0.0f, game::eye_height_for(player), 0.0f};
                const glm::vec3 aim = game::view_direction(command.yaw, command.pitch);
                const float spread = glm::radians(weapon_config.spread_degrees);

                // Same pellet loop the server runs, so offline practice and
                // online play behave identically.
                for (int pellet = 0; pellet < weapon_config.pellets; ++pellet) {
                    const std::uint32_t seed =
                        game::hash_combine(game::hash_combine(input_sequence, 0xC0FFEEu),
                                           static_cast<std::uint32_t>(pellet));
                    const glm::vec3 dir = game::spread_direction(aim, spread, seed);

                    float max_t = weapon_config.range;
                    const auto wall = world.raycast(eye, dir, weapon_config.range);
                    if (wall) {
                        max_t = wall->distance;
                    }
                    Target* hit_target = nullptr;
                    float hit_t = max_t;
                    for (Target& target : targets) {
                        if (!target.alive()) {
                            continue;
                        }
                        const auto t = game::ray_vertical_capsule(eye, dir, target.position,
                                                                  kTargetRadius, kTargetHeight);
                        if (t && *t < hit_t) {
                            hit_t = *t;
                            hit_target = &target;
                        }
                    }
                    tracers.push_back({eye + dir * 0.4f - glm::vec3{0.0f, 0.06f, 0.0f},
                                       eye + dir * hit_t, 0.08f});
                    if (hit_target != nullptr) {
                        emit_blood(particles, eye + dir * hit_t, -dir, seed);
                    } else if (wall) {
                        // Only a wall hit has a surface to spark off; a shot
                        // that reaches its max range hits nothing at all.
                        emit_impact(particles, eye + dir * hit_t, wall->normal, seed);
                    }
                    if (hit_target != nullptr) {
                        const bool killed =
                            game::apply_damage(hit_target->health, weapon_config.damage);
                        damage_numbers.push_back(
                            {hit_target->position + glm::vec3{0.0f, kTargetHeight * 0.7f, 0.0f},
                             weapon_config.damage, 1.1f});
                        hitmarker = {0.18f, killed};
                        // The two sounds below are the hitmarker in audio
                        // form -- confirmation that YOUR shot landed, not a
                        // noise the target made -- so they stay head-relative
                        // and at a fixed volume. The distance ramp they used
                        // to carry only made far hits, the ones hardest to
                        // see land, the hardest to hear land too.
                        if (killed) {
                            hit_target->respawn_remaining = kTargetRespawnSeconds;
                            ++kills;
                            emit_death_burst(
                                particles,
                                hit_target->position + glm::vec3{0.0f, kTargetHeight * 0.5f, 0.0f},
                                seed);
                            sound("kill.wav", 0.8f);
                        } else {
                            sound("hit.wav", 0.8f);
                        }
                    }
                }
            }

            // --- targets --------------------------------------------------
            for (std::size_t t = 0; t < targets.size(); ++t) {
                Target& target = targets[t];
                if (!target.alive()) {
                    target.respawn_remaining -= game::kTickSeconds;
                    if (target.alive()) {
                        game::reset_health(target.health);
                        target.position = target.home;
                    }
                    continue;
                }
                if (target.patrol_radius > 0.0f) {
                    const float phase = static_cast<float>(sim_time) * 0.8f + target.patrol_phase;
                    target.position =
                        target.home + glm::vec3{std::sin(phase) * target.patrol_radius, 0.0f, 0.0f};
                }
            }
        }
        for (Tracer& tracer : tracers) {
            tracer.ttl -= static_cast<float>(dt);
        }
        std::erase_if(tracers, [](const Tracer& tracer) { return tracer.ttl <= 0.0f; });
        // Particles advance on the render clock, not the fixed tick: they
        // are cosmetic, so they should stay smooth rather than deterministic.
        particles.update(static_cast<float>(dt));
        hitmarker.ttl = std::max(0.0f, hitmarker.ttl - static_cast<float>(dt));
        for (DamageNumber& number : damage_numbers) {
            number.ttl -= static_cast<float>(dt);
            number.world.y += static_cast<float>(dt) * 0.6f;  // float upward
        }
        std::erase_if(damage_numbers, [](const DamageNumber& n) { return n.ttl <= 0.0f; });
        if (audio) {
            audio->update();
        }

        if (online) {
            // Combat events -> visuals and audio.
            for (const auto& fire : net->take_fire_events()) {
                // One trigger pull = one event with N pellet rays: N tracers,
                // a single bang.
                bool any_hit = false;
                std::uint32_t ray_index = 0;
                const bool own_shot = fire.shooter == net->my_id();
                for (const game::FireRay& ray : fire.rays) {
                    tracers.push_back({fire.from + glm::vec3{0.0f, -0.06f, 0.0f}, ray.to, 0.08f});
                    any_hit = any_hit || ray.hit_player != game::kNoPlayer;

                    const glm::vec3 direction = ray.to - fire.from;
                    const float length = glm::length(direction);
                    if (length > 1e-4f) {
                        const glm::vec3 unit = direction / length;
                        const std::uint32_t seed =
                            game::hash_combine(fire.shooter * 2654435761u + ray_index,
                                               static_cast<std::uint32_t>(client_tick));
                        if (ray.hit_player != game::kNoPlayer) {
                            emit_blood(particles, ray.to, -unit, seed);
                        } else {
                            // The server sends only the endpoint, not the
                            // surface normal, so spray back along the ray.
                            emit_impact(particles, ray.to, -unit, seed);
                        }
                        // Your own flash comes off the viewmodel's muzzle node
                        // below; fire.from is the SERVER's copy of your eye and
                        // is tens of centimetres from your camera, which would
                        // put your own flash somewhere your gun is not. Remote
                        // shooters have no viewmodel to hang off, so they keep
                        // the eye-plus-0.3 m estimate until a weapon is
                        // attached to their hand joint.
                        if (ray_index == 0 && !own_shot) {
                            emit_muzzle_flash(particles, fire.from + unit * 0.3f, unit, seed);
                        }
                    }
                    ++ray_index;
                }
                if (own_shot) {
                    // Your own rifle is in your hands, not out in the arena.
                    // Being at zero distance would not save it either:
                    // fire.from is the SERVER's copy of your eye, which
                    // prediction leaves tens of centimetres from your camera,
                    // and panning works off the normalized direction -- so a
                    // 20 cm error pans as hard as a 20 m one, and your own
                    // gun would wander between your ears as you moved.
                    sound(fire_sound_for(fire.slot), 0.9f);
                    // Kick and flash the gun you are actually holding. The
                    // event is the server's answer, so this lands one round
                    // trip late -- the same latency the bang has always had.
                    fire_viewmodel(viewmodel, arsenal.at(fire.slot));
                    muzzle_flash_armed = true;
                    muzzle_flash_seed =
                        game::hash_combine(fire.shooter, static_cast<std::uint32_t>(client_tick));
                } else {
                    // The one that matters. Distance used to be faked here
                    // with a hand-rolled volume ramp, which told you a shot
                    // was far away but never which way to turn; the muzzle
                    // position now drives real panning and attenuation.
                    sound_at(fire_sound_for(fire.slot), fire.from, 0.9f);
                }
                if (own_shot && any_hit) {
                    sound("hit.wav", 0.8f);  // hit confirm
                    hitmarker = {0.18f, false};
                }
            }
            for (const auto& damage : net->take_damage_events()) {
                // Remembered so the kill feed can call a killing blow a
                // headshot: PlayerDied carries who, not where, and the damage
                // that caused it always arrives first on the same reliable
                // channel.
                if (damage.victim < game::kMaxPlayers) {
                    last_damage_zone[damage.victim] = damage.zone;
                }
                if (damage.attacker == net->my_id() && damage.victim != net->my_id()) {
                    const auto victim = net->players().find(damage.victim);
                    if (victim != net->players().end()) {
                        damage_numbers.push_back(
                            {victim->second.position + glm::vec3{0.0f, 1.4f, 0.0f}, damage.amount,
                             1.1f});
                    }
                    if (damage.zone == game::HitZone::Head) {
                        hitmarker.headshot = true;  // upgrades the flash the fire event started
                    }
                }
            }
            for (const auto& death : net->take_death_events()) {
                const bool headshot = death.victim < game::kMaxPlayers &&
                                      last_damage_zone[death.victim] == game::HitZone::Head;
                kill_feed.push_back({death.killer, death.victim, player_name(death.killer),
                                     player_name(death.victim), 5.0f, headshot});
                if (kill_feed.size() > 5) {
                    kill_feed.pop_front();
                }
                if (death.victim == net->my_id()) {
                    sound("death.wav", 0.9f);
                } else if (death.killer == net->my_id()) {
                    sound("kill.wav", 0.8f);
                    hitmarker = {0.35f, true, headshot};
                }
            }
            for (const auto& respawn : net->take_respawn_events()) {
                if (respawn.player == net->my_id()) {
                    prediction->reset(respawn.position);
                    player = prediction->state();
                    previous_player = player;
                }
            }
            for (KillFeedEntry& entry : kill_feed) {
                entry.ttl -= static_cast<float>(dt);
            }
            std::erase_if(kill_feed, [](const KillFeedEntry& e) { return e.ttl <= 0.0f; });

            // Reconciliation: rewind to the authoritative state and replay
            // unacked inputs.
            if (const auto ack = net->take_self_ack()) {
                const auto result = prediction->reconcile(
                    ack->position, ack->velocity, ack->on_ground, ack->last_processed_input);
                last_reconcile_error = result.error_meters;
                prediction_error_history[prediction_error_cursor] = result.error_meters;
                prediction_error_cursor =
                    (prediction_error_cursor + 1) % prediction_error_history.size();
                player = prediction->state();
                previous_player = prediction->previous_state();
                if (net->server_tick() % 300 < game::kSnapshotDivisor) {
                    eng::log::debug(
                        "net: rtt={}ms pred_err={:.4f}m pending={} interp_buffer_tick={:.1f}",
                        net->rtt_ms(), result.error_meters, prediction->pending().size(),
                        remote_render_tick);
                }
            }
            prediction->update_smoothing(static_cast<float>(dt));

            // Remote render time trails the newest snapshot; advance at tick
            // rate and gently slew toward the target to absorb jitter.
            const double target_tick =
                static_cast<double>(net->server_tick()) - game::kInterpolationDelayTicks;
            if (remote_render_tick < 0.0) {
                remote_render_tick = target_tick;
            } else {
                remote_render_tick += dt * game::kTickRate;
                remote_render_tick += (target_tick - remote_render_tick) * std::min(1.0, dt * 4.0);
            }
        }

        // Your own boots. On the render clock and outside the tick loop above
        // on purpose: this reads the simulation and never writes to it, so
        // prediction and the replay stay bit-exact whatever the frame rate.
        // Head-relative and quiet -- they are confirmation that you are
        // moving, not information about where anyone is, and at full weight
        // they would mask the steps that ARE information.
        if (!fly_mode && (mode == Mode::Offline || (online && net->self_alive()))) {
            const game::FootstepEvent footfall =
                game::update_footsteps(self_footsteps, player.position, player.velocity,
                                       player.on_ground, player.crouching);
            if (footfall.stepped) {
                sound(kStepSounds[footfall.variant], footfall.gain * kSelfStepVolume,
                      footfall.pitch);
            }
            if (footfall.landed) {
                sound("land.wav", footfall.land_gain * kSelfStepVolume);
            }
        }

        eng::Camera camera;
        if (mode == Mode::Menu) {
            // Slow orbit around the arena behind the menu.
            const float angle = static_cast<float>(clock.elapsed()) * 0.15f;
            camera.position = {std::sin(angle) * 24.0f, 12.0f, std::cos(angle) * 24.0f};
            const glm::vec3 to_center = glm::normalize(-camera.position);
            camera.yaw = std::atan2(to_center.x, -to_center.z);
            camera.pitch = std::asin(to_center.y);
        } else if (mode == Mode::Replay && playback->follow >= 0 &&
                   playback->actors[static_cast<std::size_t>(playback->follow)].active) {
            // Chase cam: behind and above the followed player, looking along
            // their facing. Not a first-person view -- the point of watching a
            // replay is usually to see what the player's own view could not.
            const ReplayActor& actor = playback->actors[static_cast<std::size_t>(playback->follow)];
            const glm::vec3 forward{std::sin(actor.yaw), 0.0f, -std::cos(actor.yaw)};
            const glm::vec3 head = actor.state.position + glm::vec3{0.0f, 1.7f, 0.0f};
            camera.position = head - forward * 5.5f + glm::vec3{0.0f, 1.3f, 0.0f};
            camera.yaw = actor.yaw;
            camera.pitch = -0.18f;
        } else if (fly_mode || mode == Mode::Replay) {
            // A replay has no local player, so the free camera is the default
            // rather than a debug toggle.
            fly.update(input, static_cast<float>(dt), window->relative_mouse());
            camera = fly.camera;
        } else if (online && !net->self_alive() && !net->kill_cam().empty()) {
            // Killcam: the seconds before this death, from the killer's eyes.
            // Driven off wall-clock rather than the simulation tick, because
            // the local player is not being simulated while dead.
            const std::vector<game::ViewSample>& trail = net->kill_cam();
            kill_cam_elapsed += static_cast<float>(dt);
            const float span = static_cast<float>(trail.size() - 1) / kKillCamPlaybackHz;
            // Holds on the final sample instead of looping. A killcam that
            // restarts reads as a bug, and the last frame -- the moment of
            // the shot -- is the one worth sitting on.
            const float t = std::min(kill_cam_elapsed, std::max(span, 0.0f));
            const float exact = t * kKillCamPlaybackHz;
            const std::size_t index = std::min(static_cast<std::size_t>(exact), trail.size() - 1);
            const std::size_t next = std::min(index + 1, trail.size() - 1);
            const float alpha = exact - static_cast<float>(index);

            const game::ViewSample& a = trail[index];
            const game::ViewSample& b = trail[next];
            camera.position = glm::mix(a.position, b.position, alpha) +
                              glm::vec3{0.0f, game::kMove.eye_height, 0.0f};
            // Shortest-way interpolation: a killer who crossed the +/-pi
            // wrap between two samples would otherwise spin the long way
            // round, which looks like the camera glitching.
            camera.yaw = a.yaw + shortest_yaw_delta(a.yaw, b.yaw) * alpha;
            camera.pitch = glm::mix(a.pitch, b.pitch, alpha);
        } else {
            const float alpha = static_cast<float>(step.alpha());
            // Ease the eye between stances so crouching doesn't snap the view.
            const float target_eye = game::eye_height_for(player);
            smoothed_eye_height +=
                (target_eye - smoothed_eye_height) * std::min(1.0f, static_cast<float>(dt) * 14.0f);
            glm::vec3 eye_pos = glm::mix(previous_player.position, player.position, alpha) +
                                glm::vec3{0.0f, smoothed_eye_height, 0.0f};
            if (online) {
                eye_pos += prediction->smoothing_offset();
            }
            camera.position = eye_pos;
            camera.yaw = view_yaw;
            camera.pitch = view_pitch;
        }
        camera.aspect = window->aspect();
        camera.fov_y_degrees = settings.fov_degrees;

        // The ears ride the camera rather than the player, so what you hear
        // always agrees with what you see -- in the killcam and the replay
        // chase cam the view is nowhere near the local player, and hearing
        // the fight from your own corpse would be worse than hearing nothing.
        // Sounds triggered earlier in this frame used the previous frame's
        // listener: one frame of head rotation, well under the ~10 degrees it
        // takes to notice a panning error.
        if (audio) {
            audio->set_listener(camera.position, camera.forward(), glm::vec3{0.0f, 1.0f, 0.0f});
        }

        // --- viewmodel ----------------------------------------------------
        // Only where the camera is genuinely the player's eye. The menu orbit,
        // the fly camera, the replay chase cam and the killcam are all looking
        // through someone else's head (or nobody's), and a gun pinned to the
        // corner of those frames would belong to no one.
        const bool viewmodel_visible =
            !fly_mode && (mode == Mode::Offline || (online && net->self_alive()));
        // The raised weapon, and the same answer the HUD gives: online that is
        // the server's, offline the local loadout's.
        const std::uint8_t viewmodel_slot = online ? net->self_weapon_slot() : loadout.slot;
        const game::WeaponConfig& viewmodel_config = arsenal.at(viewmodel_slot);
        const WeaponModel* held = weapon_model_for(arsenal.clamp_slot(viewmodel_slot));
        glm::mat4 viewmodel_model{1.0f};
        if (viewmodel_visible) {
            const bool reloading =
                online ? net->self_reloading() : loadout.weapons[loadout.slot].reloading();
            update_viewmodel(viewmodel, viewmodel_config, viewmodel_slot, reloading,
                             player.position, player.velocity, player.on_ground,
                             game::kMove.max_speed, camera.yaw, camera.pitch,
                             static_cast<float>(dt));
            viewmodel_model = viewmodel_transform(
                viewmodel, camera, held != nullptr ? held->reach : 0.0f, viewmodel_config.melee);
        } else {
            // Hidden means dead, in the menu, or watching someone else. Forget
            // everything, so coming back raises the weapon rather than
            // resuming a clip from before the death, and so the first frame
            // back does not read a respawn across the arena as a stride.
            viewmodel = ViewmodelState{};
        }
        // The flash owed by this frame's shots, now that the barrel has a
        // place to be. A weapon with no `muzzle` node -- the knife -- simply
        // disarms it: no muzzle, no flash, and no branch on which slot is
        // which. Emit direction is the weapon's own -Z, so the flash leaves
        // along the barrel rather than along the aim it just kicked off.
        if (muzzle_flash_armed) {
            if (viewmodel_visible && held != nullptr && held->muzzle) {
                emit_muzzle_flash(particles,
                                  glm::vec3(viewmodel_model * glm::vec4{*held->muzzle, 1.0f}),
                                  -glm::vec3(viewmodel_model[2]), muzzle_flash_seed);
            }
            muzzle_flash_armed = false;
        }

        // --- render -----------------------------------------------------
        // Everything drawable is collected once, then submitted twice: from
        // the sun for the shadow map, then from the camera. Building the
        // list once is what keeps the two passes from drifting apart -- a
        // caster missing from one of them is the classic shadow bug.
        draw_items.clear();
        joint_pool.clear();
        scene.each([&](eng::EntityId, eng::Entity& entity) {
            if (!entity.visible || entity.mesh < 0) {
                return;
            }
            draw_items.push_back({entity.transform.to_matrix(), DrawKind::ArenaMesh, entity.mesh,
                                  glm::vec3(entity.tint), -1, 0});
        });

        // Offline: an animated mannequin, so there is something skinned to
        // look at without a server. It cycles idle -> run -> idle so both the
        // blend and the cycle rate are visible standing still.
        if (mode == Mode::Offline) {
            const float phase = std::fmod(static_cast<float>(clock.elapsed()), 8.0f);
            const glm::vec3 fake_velocity =
                phase < 4.0f ? glm::vec3{0.0f, 0.0f, -5.5f} : glm::vec3{0.0f};
            update_character_animation(dummy_animation, fake_velocity, true, game::kMove.max_speed,
                                       static_cast<float>(dt));
            const int offset = append_character_pose(dummy_animation, joint_pool);
            // Offline has no player ids, so the mannequin borrows slot 0's
            // colour: the practice range should look like the game, and there
            // is nobody else here for it to be confused with.
            draw_items.push_back({glm::translate(glm::mat4{1.0f}, dummy_position),
                                  DrawKind::Character, -1, game::player_color(0), offset,
                                  static_cast<int>(character_skeleton->joint_count())});
        }

        // Replayed players: re-simulated, so drawn straight from their
        // current state with no interpolation needed.
        if (mode == Mode::Replay) {
            for (std::uint8_t id = 0; id < game::kMaxPlayers; ++id) {
                const ReplayActor& actor = playback->actors[id];
                if (!actor.active) {
                    continue;
                }
                glm::mat4 model = glm::translate(glm::mat4{1.0f}, actor.state.position);
                model = glm::rotate(model, actor.yaw, glm::vec3{0.0f, 1.0f, 0.0f});

                CharacterAnimation& animation = replay_animations[id];
                update_character_animation(animation, actor.state.velocity, actor.state.on_ground,
                                           game::kMove.max_speed, static_cast<float>(dt));
                const int offset = append_character_pose(animation, joint_pool);
                draw_items.push_back({model, DrawKind::Character, -1, game::player_color(id),
                                      offset, static_cast<int>(character_skeleton->joint_count())});
            }
        }

        // Remote players (online): interpolated ~100 ms in the past.
        if (online) {
            for (const auto& [id, remote] : net->players()) {
                if (id == net->my_id() || remote.history.empty()) {
                    continue;
                }
                const auto pose = remote.history.sample(remote_render_tick);
                if (!pose || (pose->flags & game::kFlagAlive) == 0) {
                    continue;  // dead players are not drawn
                }
                // The character's feet sit at its origin, so the snapshot
                // position is used directly -- no half-height offset like the
                // stretched cube needed. Yaw comes from the snapshot, so a
                // player faces the way they are looking.
                glm::mat4 model = glm::translate(glm::mat4{1.0f}, pose->position);
                model = glm::rotate(model, pose->yaw, glm::vec3{0.0f, 1.0f, 0.0f});

                // Snapshots carry no velocity, so it is estimated from two
                // interpolated samples. That is only used to pick a clip and
                // a playback rate; nothing depends on it being exact.
                const auto earlier = remote.history.sample(remote_render_tick - 4.0);
                glm::vec3 velocity{0.0f};
                if (earlier) {
                    velocity = (pose->position - earlier->position) / (4.0f * game::kTickSeconds);
                }
                CharacterAnimation& animation = remote_animations[id];
                update_character_animation(animation, velocity, std::abs(velocity.y) < 0.6f,
                                           game::kMove.max_speed, static_cast<float>(dt));

                // Someone else's boots, at their feet. This is the entire
                // point of the feature: hearing WHERE an enemy is, from the
                // same interpolated position they are drawn at, so what you
                // hear and what you see cannot disagree. on_ground and
                // crouching ride in the snapshot flags, so a player who
                // crouches really is quieter to everyone else, with no
                // protocol field of its own.
                const game::FootstepEvent footfall =
                    game::update_footsteps(remote_footsteps[id], pose->position, velocity,
                                           (pose->flags & game::kFlagOnGround) != 0,
                                           (pose->flags & game::kFlagCrouching) != 0);
                if (footfall.stepped) {
                    sound_at(kStepSounds[footfall.variant], pose->position, footfall.gain,
                             footfall.pitch);
                }
                if (footfall.landed) {
                    sound_at("land.wav", pose->position, footfall.land_gain);
                }

                const int offset = append_character_pose(animation, joint_pool);
                draw_items.push_back({model, DrawKind::Character, -1, game::player_color(id),
                                      offset, static_cast<int>(character_skeleton->joint_count())});
            }
        }

        // Targets: stretched cubes colored by remaining health.
        for (const Target& target : targets) {
            if (!target.alive()) {
                continue;
            }
            glm::mat4 model = glm::translate(
                glm::mat4{1.0f}, target.position + glm::vec3{0.0f, kTargetHeight * 0.5f, 0.0f});
            model = glm::scale(model, {kTargetRadius * 2.0f, kTargetHeight, kTargetRadius * 2.0f});
            const float hp = target.health.current / target.health.max;
            draw_items.push_back(
                {model, DrawKind::Cube, -1, {0.9f, 0.15f + 0.6f * hp, 0.15f}, -1, 0});
        }

        int draw_calls = 0;

        // Window resizes have to reach the off-screen targets too, or the
        // post chain keeps resolving at the old size and the image stretches.
        // Skipped while the drawable is degenerate -- the browser canvas
        // reports 0x0 until it is laid out, and reallocating every frame
        // against that would churn for nothing.
        if (window->width_px() > 0 && window->height_px() > 0 &&
            (window->width_px() != postfx->width() || window->height_px() != postfx->height())) {
            postfx->resize(window->width_px(), window->height_px());
        }

        // Uploads a draw item's joint matrices, or turns skinning off. Used
        // by both passes so they read the same slice of the pool.
        const auto bind_joints = [&](const eng::Shader& shader, const DrawItem& item) {
            const bool skinned = item.joint_offset >= 0 && item.joint_count > 0;
            shader.set_int("u_skinned", skinned ? 1 : 0);
            shader.set_int("u_joint_texture", 2);
            if (skinned) {
                joint_texture->upload_and_bind(
                    std::span<const glm::mat4>{
                        joint_pool.data() + static_cast<std::size_t>(item.joint_offset),
                        static_cast<std::size_t>(item.joint_count)},
                    2);
            }
        };

        // Pass 1: depth from the sun. Front faces are culled so the depth
        // recorded is the caster's back face, which pushes self-shadowing
        // acne to surfaces that are facing away from the light anyway.
        depth_shader->bind();
        depth_shader->set_mat4("u_light_view_projection", light_view_projection);
        shadow_map->begin_depth_pass();
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        for (const DrawItem& item : draw_items) {
            depth_shader->set_mat4("u_model", item.model);
            bind_joints(*depth_shader, item);
            switch (item.kind) {
                case DrawKind::Character:
                    character_mesh.draw();
                    break;
                case DrawKind::Cube:
                    cube.draw();
                    break;
                case DrawKind::ArenaMesh:
                    for (const RenderPrimitive& primitive :
                         render_meshes[static_cast<std::size_t>(item.mesh)]) {
                        primitive.gpu.draw();
                    }
                    break;
            }
        }
        glCullFace(GL_BACK);
        glDisable(GL_CULL_FACE);
        shadow_map->end_depth_pass(window->width_px(), window->height_px());

        // Pass 2: lit, from the camera, into the HDR post-processing target.
        // The clear colour is black and is never meant to be seen: the sky
        // pass below fills every pixel the world does not, so flat black in a
        // frame means something went wrong rather than "that is the
        // background".
        postfx->begin_scene({0.0f, 0.0f, 0.0f, 1.0f});

        const glm::mat4 view_projection = camera.view_projection();

        lit_shader->bind();
        lit_shader->set_mat4("u_view_projection", view_projection);
        lit_shader->set_vec3("u_light_direction", glm::normalize(kSunDirection));
        lit_shader->set_vec3("u_light_color", {1.0f, 0.97f, 0.9f});
        lit_shader->set_vec3("u_ambient", {0.20f, 0.22f, 0.26f});
        lit_shader->set_vec3("u_camera_pos", camera.position);
        lit_shader->set_int("u_base_color", 0);
        lit_shader->set_int("u_shadow_map", 1);
        lit_shader->set_int("u_normal_map", 3);
        lit_shader->set_mat4("u_light_view_projection", light_view_projection);
        lit_shader->set_float("u_shadow_texel",
                              1.0f / static_cast<float>(shadow_map->resolution()));
        shadow_map->bind_depth(1);

        // Texture binds are the only per-draw GL state here, so track the
        // current one and only switch when the material actually changes.
        // Unit 2 belongs to the joint texture, so the normal map takes 3.
        int bound_texture = -1;         // -1 = the white fallback
        int bound_normal_texture = -1;  // -1 = the flat-normal fallback
        white.bind(0);
        flat_normal.bind(3);
        const auto bind_material = [&](int texture, int normal_texture, float normal_scale) {
            if (texture != bound_texture) {
                bound_texture = texture;
                if (texture < 0) {
                    white.bind(0);
                } else {
                    arena_textures[static_cast<std::size_t>(texture)].bind(0);
                }
            }
            if (normal_texture != bound_normal_texture) {
                bound_normal_texture = normal_texture;
                if (normal_texture < 0) {
                    flat_normal.bind(3);
                } else {
                    arena_textures[static_cast<std::size_t>(normal_texture)].bind(3);
                }
            }
            lit_shader->set_float("u_normal_strength", normal_texture < 0 ? 0.0f : normal_scale);
        };

        for (const DrawItem& item : draw_items) {
            lit_shader->set_mat4("u_model", item.model);
            lit_shader->set_mat3("u_normal_matrix",
                                 glm::mat3(glm::transpose(glm::inverse(item.model))));
            bind_joints(*lit_shader, item);
            if (item.kind != DrawKind::ArenaMesh) {
                // Characters and targets are untextured, so they get the
                // white albedo and the flat normal.
                bind_material(-1, -1, 1.0f);
                lit_shader->set_vec3("u_tint", item.tint);
                if (item.kind == DrawKind::Character) {
                    character_mesh.draw();
                } else {
                    cube.draw();
                }
                ++draw_calls;
                continue;
            }
            for (const RenderPrimitive& primitive :
                 render_meshes[static_cast<std::size_t>(item.mesh)]) {
                bind_material(primitive.texture, primitive.normal_texture, primitive.normal_scale);
                lit_shader->set_vec3("u_tint", primitive.color * item.tint);
                primitive.gpu.draw();
                ++draw_calls;
            }
        }

        // Sky last of the opaque work, not first. Its triangle sits exactly
        // on the far plane (gl_Position.z == w, depth func LEQUAL), so the
        // depth test throws it away wherever the arena already covered the
        // pixel and it only ever shades sky you can actually see. Drawing it
        // first with depth writes off would be a line shorter and would shade
        // every pixel in the frame, most of them for the world to paint over
        // immediately. It goes into the HDR target with the rest of the
        // scene, which is what lets the sun feed bloom.
        sky->draw(view_projection, camera.position, glm::normalize(kSunDirection), sky_params);
        ++draw_calls;

        // Particles after the opaque scene so they blend against it, and
        // before the debug lines so tracers stay readable through smoke.
        particle_renderer->draw(particles, view_projection, camera.right(),
                                glm::cross(camera.right(), camera.forward()));

        for (const Tracer& tracer : tracers) {
            debug_draw->line(tracer.from, tracer.to, {1.0f, 0.9f, 0.4f});
        }

        // The local player does not exist in a replay, so its capsule and aim
        // ray would just be debug clutter sitting at the default spawn.
        if (draw_physics && mode != Mode::Replay) {
            draw_capsule(
                *debug_draw, player.position, controller.config().radius,
                controller.config().height,
                player.on_ground ? glm::vec3{0.2f, 1.0f, 0.3f} : glm::vec3{1.0f, 0.6f, 0.1f});
            debug_draw->line(player.position, player.position + player.velocity * 0.3f,
                             {1.0f, 1.0f, 0.2f});
            // Aim ray from the eye.
            const glm::vec3 eye = camera.position;
            if (const auto hit = world.raycast(eye, camera.forward(), 100.0f)) {
                debug_draw->line(eye, hit->position, {0.9f, 0.2f, 0.2f});
                debug_draw->line(hit->position, hit->position + hit->normal * 0.5f,
                                 {0.2f, 0.6f, 1.0f});
            }
        }
        debug_draw->axes(glm::mat4{1.0f}, 2.0f);
        draw_calls += debug_draw->flush(view_projection) > 0 ? 1 : 0;

        // Pass 3: the first-person weapon, still inside the HDR target so its
        // muzzle flash reaches bloom like everything else in the scene.
        //
        // Depth is CLEARED and the gun drawn with its own projection -- same
        // FOV and aspect, near plane 0.01 instead of the camera's 0.05. That
        // is the whole anti-clipping mechanism: after the clear there is no
        // world depth left to lose against, so a player who walks into a wall
        // gets a gun in front of the wall rather than a barrel through it, and
        // the tight near plane keeps the stock (which lives inside the
        // player's head, as it does in every FPS) from clipping. The FOV MUST
        // match: a point projects to the same pixel in both matrices only if
        // the x/y scale agrees, and if it does not, the flash lands somewhere
        // other than the drawn muzzle.
        //
        // Deliberately AFTER the particles and debug lines, not before: they
        // still need the world's depth buffer to be occluded by the world, and
        // clearing it first would put impact sparks through pillars. The cost
        // is that the gun overdraws the part of its own flash that falls on
        // the barrel in screen space -- which is what a real flash does
        // anyway, since that part is physically behind the barrel.
        //
        // It is not in draw_items, so it casts NO shadow: a first-person gun
        // is not in the world, and a gun-shaped shadow on the floor beside a
        // player who cannot see their own body would be worse than none. It
        // does RECEIVE the world's shadows, because its transform is a genuine
        // world position -- so the gun goes dark when you step into a pillar's
        // shade, for free.
        if (viewmodel_visible && held != nullptr) {
            glClear(GL_DEPTH_BUFFER_BIT);
            const glm::mat4 viewmodel_projection = glm::perspective(
                glm::radians(camera.fov_y_degrees), camera.aspect, kViewmodelNear, kViewmodelFar);
            lit_shader->bind();
            lit_shader->set_mat4("u_view_projection", viewmodel_projection * camera.view());
            // The rest of the lit uniforms are program state and survive from
            // pass 2. These do not, because pass 2 leaves them wherever its
            // last draw put them: the last item may have been a skinned
            // character, and unit 0 and unit 3 may still hold whichever arena
            // material was bound last. The normal map matters more than it
            // looks -- the loader derives a tangent frame for every mesh with
            // UVs, so a weapon box has a perfectly good one and would happily
            // wear the concrete's bumps at whatever strength the wall left
            // behind. Strength 0 is the "no map" contract bind_material uses.
            lit_shader->set_int("u_skinned", 0);
            lit_shader->set_float("u_normal_strength", 0.0f);
            white.bind(0);
            flat_normal.bind(3);
            shadow_map->bind_depth(1);
            for (const WeaponModel::Box& box : held->boxes) {
                const glm::mat4 model = viewmodel_model * box.local;
                lit_shader->set_mat4("u_model", model);
                lit_shader->set_mat3("u_normal_matrix",
                                     glm::mat3(glm::transpose(glm::inverse(model))));
                lit_shader->set_vec3(
                    "u_tint", held->primitives[static_cast<std::size_t>(box.primitive)].color);
                held->primitives[static_cast<std::size_t>(box.primitive)].gpu.draw();
                ++draw_calls;
            }
        }

        // Resolve the HDR scene to the screen. Everything below this point
        // (HUD, ImGui) draws straight to the default framebuffer, so it is
        // never tonemapped, bloomed or blurred -- text must stay crisp.
        postfx->resolve(postfx_settings);
        draw_calls += postfx->last_pass_count();

        // --- debug UI -----------------------------------------------------
        imgui->begin_frame();
        frame_ms_history[frame_ms_cursor] = static_cast<float>(dt * 1000.0);
        frame_ms_cursor = (frame_ms_cursor + 1) % frame_ms_history.size();

        // --- main menu ---------------------------------------------------
        if (mode == Mode::Menu) {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos({display_size.x * 0.5f, display_size.y * 0.45f},
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({420, 0}, ImGuiCond_Always);
            ImGui::Begin(
                "FPS", nullptr,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            ImGui::InputText("name", menu_name, sizeof(menu_name));
            ImGui::InputText("server address", menu_ip, sizeof(menu_ip));

            const auto start_session = [&](Mode next) {
                settings.name = menu_name[0] != '\0' ? menu_name : "player";
                settings.last_ip = menu_ip;
                save_settings(settings);
                menu_error.clear();
                player = {};
                player.position = spawn;
                previous_player = player;
                controller.set_position(spawn);
                controller.set_velocity({0.0f, 0.0f, 0.0f});
                view_yaw = 0.0f;
                view_pitch = 0.0f;
                input_sequence = 0;
                mode = next;
                online = next == Mode::Online;
                window->set_relative_mouse(true);
            };

            // Native: ENet/UDP to host:port. Browser: WebSocket to the
            // address (a ws://host:port or wss://domain URL).
            if (ImGui::Button("Connect", {200, 0})) {
                reconnect.stop_waiting();  // a manual connect overrides the wait
                reconnect.host = menu_ip;
                net = game::NetClient::connect(menu_ip, args.port, menu_name);
                if (net) {
                    prediction.emplace(world, spawn);
                    start_session(Mode::Online);
                } else {
                    menu_error = "could not start a connection (bad address?)";
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Practice offline", {200, 0})) {
                reconnect.stop_waiting();
                start_session(Mode::Offline);
            }
            if (!menu_error.empty()) {
                ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", menu_error.c_str());
            }

            // Waiting out a protocol mismatch: count down, then try again.
            if (reconnect.waiting()) {
                reconnect.seconds_until -= static_cast<float>(dt);
                ImGui::TextColored({1.0f, 0.8f, 0.3f, 1.0f},
                                   "Server is updating - retrying in %.0fs (attempt %d)",
                                   std::max(0.0f, reconnect.seconds_until), reconnect.attempts);
#if defined(__EMSCRIPTEN__)
                // Retrying alone cannot fix this half of it. The wasm this
                // page is running was compiled against the older protocol and
                // will keep being rejected no matter how many times it asks;
                // only fetching the newly published client helps, and that
                // means a page load. Offered as a button rather than done
                // automatically: an auto-reload that races a deploy still in
                // flight would just reload into the same client, repeatedly,
                // and a page that reloads itself under a player is worse than
                // one that asks.
                ImGui::TextWrapped(
                    "This page is running an older client than the server. Reload to fetch "
                    "the updated one.");
                if (ImGui::Button("Reload for the updated client", {260, 0})) {
                    emscripten_run_script("location.reload()");
                }
#endif
                if (reconnect.seconds_until <= 0.0f) {
                    net = game::NetClient::connect(reconnect.host, args.port, menu_name);
                    if (net) {
                        prediction.emplace(world, spawn);
                        start_session(Mode::Online);
                    } else {
                        // Could not even open a socket -- the server is
                        // probably mid-restart. Same wait, same message.
                        ++reconnect.attempts;
                        reconnect.seconds_until = retry_delay(reconnect.attempts);
                    }
                }
            }

            ImGui::SeparatorText("settings");
            ImGui::SliderFloat("sensitivity", &settings.sensitivity, 0.0005f, 0.01f, "%.4f");
            ImGui::SliderFloat("field of view", &settings.fov_degrees, 50.0f, 120.0f, "%.0f");
            if (ImGui::SliderFloat("volume", &settings.volume, 0.0f, 1.0f) && audio) {
                audio->set_master_volume(settings.volume);
            }

            ImGui::Separator();
            if (ImGui::Button("Quit")) {
                running = false;
            }
            ImGui::End();
        }

        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Debug");
        ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                    1000.0f / ImGui::GetIO().Framerate);
        ImGui::PlotLines("frame ms", frame_ms_history.data(),
                         static_cast<int>(frame_ms_history.size()),
                         static_cast<int>(frame_ms_cursor), nullptr, 0.0f, 33.3f, {220, 60});
        ImGui::Text("draw calls: %d", draw_calls);
        ImGui::Text("pos: (%.2f, %.2f, %.2f)", player.position.x, player.position.y,
                    player.position.z);
        ImGui::Text("vel: (%.2f, %.2f, %.2f) |h|=%.2f", player.velocity.x, player.velocity.y,
                    player.velocity.z, std::hypot(player.velocity.x, player.velocity.z));
        ImGui::Text("on_ground: %s", player.on_ground ? "yes" : "no");
        if (online) {
            ImGui::Separator();
            ImGui::Text("net: %s | id %u | rtt %u ms", net->state_name(), net->my_id(),
                        net->rtt_ms());
            ImGui::Text("server tick %u | acked input %u | pending %zu", net->server_tick(),
                        net->last_processed_input(), prediction->pending().size());
            ImGui::Text("players %zu | rx %llu B tx %llu B", net->players().size(),
                        static_cast<unsigned long long>(net->stats().bytes_received),
                        static_cast<unsigned long long>(net->stats().bytes_sent));
            ImGui::Text("prediction error: %.4f m", last_reconcile_error);
            ImGui::PlotLines("pred err", prediction_error_history.data(),
                             static_cast<int>(prediction_error_history.size()),
                             static_cast<int>(prediction_error_cursor), nullptr, 0.0f, 0.5f,
                             {220, 50});
            if (ImGui::CollapsingHeader("network simulation")) {
                ImGui::SliderInt("latency ms (one-way)", &sim_config.latency_ms, 0, 300);
                ImGui::SliderInt("jitter ms", &sim_config.jitter_ms, 0, 100);
                ImGui::SliderFloat("loss %%", &sim_config.loss_percent, 0.0f, 30.0f);
            }
        }
        ImGui::Checkbox("physics debug (F3)", &draw_physics);
        ImGui::Checkbox("fly mode (F1)", &fly_mode);

        if (mode == Mode::Replay) {
            ImGui::Separator();
            const std::size_t total = playback->replay.frames.size();
            ImGui::Text("replay: frame %zu / %zu (tick %u)", playback->next_frame, total,
                        playback->current_tick());
            if (ImGui::Button(playback->paused ? "play" : "pause")) {
                playback->paused = !playback->paused;
            }
            ImGui::SameLine();
            if (ImGui::Button("restart")) {
                reset_playback(*playback, world);
                replay_animations.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("step") && playback->paused) {
                step_playback(*playback, world);
            }
            ImGui::SliderFloat("speed", &playback->speed, 0.1f, 4.0f);

            // Follow list. -1 is the free camera.
            if (ImGui::BeginCombo("camera",
                                  playback->follow < 0
                                      ? "free"
                                      : playback->actors[static_cast<std::size_t>(playback->follow)]
                                            .name.c_str())) {
                if (ImGui::Selectable("free", playback->follow < 0)) {
                    playback->follow = -1;
                }
                for (std::uint8_t id = 0; id < game::kMaxPlayers; ++id) {
                    if (!playback->actors[id].active) {
                        continue;
                    }
                    const bool selected = playback->follow == static_cast<int>(id);
                    if (ImGui::Selectable(playback->actors[id].name.c_str(), selected)) {
                        playback->follow = static_cast<int>(id);
                    }
                }
                ImGui::EndCombo();
            }
            if (playback->finished()) {
                ImGui::TextColored({1.0f, 0.8f, 0.3f, 1.0f}, "end of replay");
            }
        }

        if (ImGui::CollapsingHeader("post-processing")) {
            ImGui::Text("scene target: %s", postfx->hdr() ? "RGBA16F" : "RGBA8 (no HDR)");
            ImGui::Checkbox("bloom", &postfx_settings.bloom);
            ImGui::SameLine();
            ImGui::Checkbox("fxaa", &postfx_settings.fxaa);
            ImGui::SliderFloat("exposure", &postfx_settings.exposure, 0.1f, 4.0f);
            ImGui::SliderFloat("bloom threshold", &postfx_settings.bloom_threshold, 0.0f, 4.0f);
            ImGui::SliderFloat("bloom knee", &postfx_settings.bloom_knee, 0.0f, 1.0f);
            ImGui::SliderFloat("bloom intensity", &postfx_settings.bloom_intensity, 0.0f, 2.0f);
        }

        if (ImGui::CollapsingHeader("sky")) {
            ImGui::ColorEdit3("horizon", &sky_params.horizon_color.x);
            ImGui::ColorEdit3("zenith", &sky_params.zenith_color.x);
            ImGui::ColorEdit3("sun", &sky_params.sun_color.x);
            // Sun intensity ranges well past 1.0 because the disc is the only
            // thing in the sky the bright pass is meant to catch.
            ImGui::SliderFloat("sun intensity", &sky_params.sun_intensity, 0.0f, 40.0f);
            ImGui::SliderFloat("sun radius", &sky_params.sun_angular_radius, 0.005f, 0.15f);
            ImGui::SliderFloat("halo intensity", &sky_params.halo_intensity, 0.0f, 0.5f);
        }
        ImGui::TextDisabled(
            "Esc: capture | WASD+Space | Shift: sprint | Ctrl: crouch | 1-4: weapon | LMB "
            "fire | R reload");
        ImGui::End();

        // --- match UI (online) ---------------------------------------------
        if (online) {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;

            // Match timer, top center.
            ImGui::SetNextWindowPos({display_size.x * 0.5f, 8.0f}, ImGuiCond_Always, {0.5f, 0.0f});
            ImGui::Begin("##timer", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
            ImGui::SetWindowFontScale(1.5f);
            ImGui::Text("%02u:%02u", net->match_seconds() / 60, net->match_seconds() % 60);
            ImGui::End();

            // Kill feed, top right.
            if (!kill_feed.empty()) {
                ImGui::SetNextWindowPos({display_size.x - 12.0f, 12.0f}, ImGuiCond_Always,
                                        {1.0f, 0.0f});
                ImGui::Begin("##killfeed", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
                for (const KillFeedEntry& entry : kill_feed) {
                    const float alpha = std::min(1.0f, entry.ttl);
                    const ImVec4 text_color{1.0f, 0.85f, 0.4f, alpha};
                    player_chip(entry.killer, alpha);
                    ImGui::TextColored(text_color, "%s killed", entry.killer_name.c_str());
                    ImGui::SameLine();
                    player_chip(entry.victim, alpha);
                    ImGui::TextColored(text_color, "%s", entry.victim_name.c_str());
                    if (entry.headshot) {
                        ImGui::SameLine();
                        ImGui::TextColored({1.0f, 0.77f, 0.25f, alpha}, "(headshot)");
                    }
                }
                ImGui::End();
            }

            // Scoreboard: held Tab, or automatically on the end screen.
            const bool match_over = net->match_phase() == game::MatchPhase::Ended;
            if (input.is_down(eng::Key::Tab) || match_over) {
                ImGui::SetNextWindowPos({display_size.x * 0.5f, display_size.y * 0.35f},
                                        ImGuiCond_Always, {0.5f, 0.5f});
                ImGui::Begin("Scoreboard", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs);
                if (match_over) {
                    ImGui::SetWindowFontScale(1.4f);
                    ImGui::Text("MATCH OVER - restarting in %us", net->match_seconds());
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::Separator();
                }
                // Sort by kills descending.
                std::vector<std::pair<std::uint8_t, game::NetClient::Scores>> rows(
                    net->scores().begin(), net->scores().end());
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                    return a.second.kills > b.second.kills;
                });
                if (ImGui::BeginTable("scores", 3, ImGuiTableFlags_Borders)) {
                    ImGui::TableSetupColumn("player");
                    ImGui::TableSetupColumn("kills");
                    ImGui::TableSetupColumn("deaths");
                    ImGui::TableHeadersRow();
                    for (const auto& [id, score] : rows) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        player_chip(id, 1.0f);
                        ImGui::Text("%s%s", player_name(id).c_str(),
                                    id == net->my_id() ? " (you)" : "");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%u", score.kills);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%u", score.deaths);
                    }
                    ImGui::EndTable();
                }

                // Career records, when the server keeps them. Below the live
                // scores and visually secondary, because the match is what
                // the player is in the middle of.
                if (!net->leaderboard().empty()) {
                    ImGui::Separator();
                    ImGui::Text("ALL-TIME");
                    // Said here, not in a README nobody opens. There are no
                    // accounts and nothing checks who typed what, so these are
                    // claims. A leaderboard that looks authoritative and is
                    // not would be worse than having none at all.
                    ImGui::TextColored({0.7f, 0.7f, 0.7f, 1.0f},
                                       "names are unverified - anyone can type any name");
                    if (ImGui::BeginTable("careers", 4, ImGuiTableFlags_Borders)) {
                        ImGui::TableSetupColumn("player");
                        ImGui::TableSetupColumn("kills");
                        ImGui::TableSetupColumn("deaths");
                        ImGui::TableSetupColumn("matches");
                        ImGui::TableHeadersRow();
                        for (const game::LeaderboardEntry& entry : net->leaderboard()) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%s", entry.name.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%u", entry.kills);
                            ImGui::TableSetColumnIndex(2);
                            ImGui::Text("%u", entry.deaths);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::Text("%u", entry.matches);
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::End();
            }

            // Death overlay.
            if (!net->self_alive() && !match_over) {
                ImGui::SetNextWindowPos({display_size.x * 0.5f, display_size.y * 0.5f},
                                        ImGuiCond_Always, {0.5f, 0.5f});
                ImGui::Begin("##dead", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
                ImGui::SetWindowFontScale(2.0f);
                ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "YOU DIED");
                // Without this the view jumping to somewhere the player was
                // not just reads as a camera bug. Naming the killer is what
                // makes it legible as a killcam.
                if (!net->kill_cam().empty()) {
                    ImGui::SetWindowFontScale(1.0f);
                    player_chip(net->kill_cam_killer(), 1.0f);
                    ImGui::TextColored({0.85f, 0.85f, 0.85f, 1.0f}, "killed by %s",
                                       player_name(net->kill_cam_killer()).c_str());
                }
                ImGui::End();
            }
        }

        // --- HUD ---------------------------------------------------------
        // Nothing here applies to a replay: there is no local player whose
        // health, ammo or score any of it could describe.
        if (!fly_mode && mode != Mode::Menu && mode != Mode::Replay) {
            ImDrawList* overlay = ImGui::GetForegroundDrawList();
            const ImVec2 center{ImGui::GetIO().DisplaySize.x * 0.5f,
                                ImGui::GetIO().DisplaySize.y * 0.5f};

            // Which weapon is actually raised: the server's answer when
            // online, our own loadout when practicing.
            const std::uint8_t hud_slot = online ? net->self_weapon_slot() : loadout.slot;
            const game::WeaponConfig& hud_weapon = arsenal.at(hud_slot);

            // Crosshair gap tracks the weapon's spread and opens up while
            // moving, so the reticle communicates actual accuracy.
            const float speed_frac =
                std::clamp(std::hypot(player.velocity.x, player.velocity.z) /
                               (game::kMove.max_speed * game::kMove.sprint_multiplier),
                           0.0f, 1.0f);
            const float gap = 4.0f + hud_weapon.spread_degrees * 1.6f + speed_frac * 5.0f;
            const ImU32 cross_color = IM_COL32(240, 240, 240, 220);
            constexpr float kArm = 9.0f;
            overlay->AddLine({center.x - gap - kArm, center.y}, {center.x - gap, center.y},
                             cross_color, 2.0f);
            overlay->AddLine({center.x + gap, center.y}, {center.x + gap + kArm, center.y},
                             cross_color, 2.0f);
            overlay->AddLine({center.x, center.y - gap - kArm}, {center.x, center.y - gap},
                             cross_color, 2.0f);
            overlay->AddLine({center.x, center.y + gap}, {center.x, center.y + gap + kArm},
                             cross_color, 2.0f);

            // Hitmarker: a short X over the crosshair. Red for a kill, amber
            // and heavier for a headshot -- the shot worth learning to repeat
            // has to be distinguishable at a glance from an ordinary hit.
            if (hitmarker.ttl > 0.0f) {
                const float fade = std::clamp(hitmarker.ttl / 0.18f, 0.0f, 1.0f);
                const int alpha = static_cast<int>((hitmarker.kill ? 255 : 230) * fade);
                ImU32 color =
                    hitmarker.kill ? IM_COL32(255, 70, 70, alpha) : IM_COL32(255, 255, 255, alpha);
                float thickness = 2.0f;
                float inner = 5.0f;
                float outer = 12.0f;
                if (hitmarker.headshot) {
                    color = IM_COL32(255, 196, 64, alpha);
                    thickness = 3.0f;
                    inner = 6.0f;
                    outer = 15.0f;
                }
                for (const auto& [sx, sy] : {std::pair{1.0f, 1.0f}, std::pair{-1.0f, 1.0f}}) {
                    overlay->AddLine({center.x + sx * inner, center.y + sy * inner},
                                     {center.x + sx * outer, center.y + sy * outer}, color,
                                     thickness);
                    overlay->AddLine({center.x - sx * inner, center.y - sy * inner},
                                     {center.x - sx * outer, center.y - sy * outer}, color,
                                     thickness);
                }
            }

            // Floating damage numbers, projected from world space.
            const glm::mat4 view_proj = camera.view_projection();
            for (const DamageNumber& number : damage_numbers) {
                const glm::vec4 clip = view_proj * glm::vec4(number.world, 1.0f);
                if (clip.w <= 0.0f) {
                    continue;  // behind the camera
                }
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                const ImVec2 screen{(ndc.x * 0.5f + 0.5f) * ImGui::GetIO().DisplaySize.x,
                                    (1.0f - (ndc.y * 0.5f + 0.5f)) * ImGui::GetIO().DisplaySize.y};
                const float fade = std::clamp(number.ttl / 1.1f, 0.0f, 1.0f);
                char text[16];
                std::snprintf(text, sizeof(text), "%.0f", number.amount);
                overlay->AddText(nullptr, 20.0f, {screen.x + 1.0f, screen.y + 1.0f},
                                 IM_COL32(0, 0, 0, static_cast<int>(180 * fade)), text);
                overlay->AddText(nullptr, 20.0f, screen,
                                 IM_COL32(255, 220, 90, static_cast<int>(255 * fade)), text);
            }

            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos({display.x * 0.5f, display.y - 20.0f}, ImGuiCond_Always,
                                    {0.5f, 1.0f});
            ImGui::Begin("##hud", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
            ImGui::SetWindowFontScale(1.6f);
            const bool hud_reloading =
                online ? net->self_reloading() : loadout.weapons[loadout.slot].reloading();
            const int hud_ammo = online ? net->self_ammo() : loadout.weapons[loadout.slot].ammo;
            const int hud_mag = online && net->self_magazine() > 0 ? net->self_magazine()
                                                                   : hud_weapon.magazine_size;
            const int hud_kills = online ? [&] {
                const auto it = net->scores().find(net->my_id());
                return it != net->scores().end() ? static_cast<int>(it->second.kills) : 0;
            }()
                                         : kills;
            const int hud_deaths = online ? [&] {
                const auto it = net->scores().find(net->my_id());
                return it != net->scores().end() ? static_cast<int>(it->second.deaths) : 0;
            }()
                                          : deaths;
            const float hud_health = online ? net->self_health() : 100.0f;

            if (hud_reloading) {
                ImGui::Text("HP %.0f    %s  RELOADING    K %d / D %d", hud_health,
                            hud_weapon.name.c_str(), hud_kills, hud_deaths);
            } else {
                ImGui::Text("HP %.0f    %s  %d / %d    K %d / D %d", hud_health,
                            hud_weapon.name.c_str(), hud_ammo, hud_mag, hud_kills, hud_deaths);
            }
            ImGui::SetWindowFontScale(1.0f);
            // Weapon slots; the raised one is highlighted.
            for (std::size_t i = 0; i < arsenal.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine();
                }
                const bool active = i == hud_slot;
                ImGui::TextColored(
                    active ? ImVec4{1.0f, 0.85f, 0.35f, 1.0f} : ImVec4{0.55f, 0.6f, 0.65f, 1.0f},
                    "[%zu] %s", i + 1, arsenal.weapons[i].name.c_str());
            }
            if (player.crouching) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 0.8f, 1.0f, 1.0f}, "  CROUCH");
            } else if (player.sprinting) {
                ImGui::SameLine();
                ImGui::TextColored({1.0f, 0.7f, 0.4f, 1.0f}, "  SPRINT");
            }
            ImGui::End();
        }
        imgui->end_frame();

        const bool last_frame = args.run_seconds && clock.elapsed() >= *args.run_seconds;

#if !defined(__EMSCRIPTEN__)
        // Grab the finished frame from the back buffer before it is swapped
        // out; after the swap its contents are undefined.
        if (last_frame && args.screenshot) {
            eng::save_framebuffer_png(*args.screenshot, window->width_px(), window->height_px());
        }
#else
        // Same "before the swap" rule as the screenshot above, and the same
        // reason not to do it every frame: the readback is synchronous and
        // would itself become the thing being measured.
        //
        // "First frame at or after N with a non-degenerate framebuffer",
        // not "frame N", because the canvas can still be 0-high that late --
        // that is the startup race the clamp in PostFx::resize exists for,
        // and fingerprinting into it would report a broken client that is
        // in fact about to come up fine.
        ++web_frames;
        if (web_distinct_colors == 0 && web_frames >= kSmokeSignatureFrame &&
            window->width_px() > 0 && window->height_px() > 0) {
            if (const auto signature =
                    eng::read_framebuffer_signature(window->width_px(), window->height_px())) {
                web_distinct_colors = signature->distinct_colors;
                web_mean_luma = signature->mean_luma;
            }
        }
        fps_publish_smoke(web_frames, clock.elapsed(), web_distinct_colors, web_mean_luma);
#endif

        window->swap();

#if defined(ENG_ENABLE_ASSERTS)
        eng::check_gl_errors("frame end");
#endif

        if (last_frame) {
            eng::log::info("--run-seconds elapsed; quitting (pos=({:.2f},{:.2f},{:.2f}))",
                           player.position.x, player.position.y, player.position.z);
            running = false;
        }
    };

#if defined(__EMSCRIPTEN__)
    // Browser drives frames; the call never returns and keeps main()'s stack
    // (and thus `frame`'s captures) alive. fps=0 -> requestAnimationFrame.
    static std::function<void()> web_frame = frame;
    emscripten_set_main_loop([] { web_frame(); }, 0, 1);
    (void)running;
#else
    while (running) {
        frame();
    }

    settings.name = menu_name[0] != '\0' ? menu_name : settings.name;
    settings.last_ip = menu_ip[0] != '\0' ? menu_ip : settings.last_ip;
    save_settings(settings);
#endif
    eng::log::info("FPS client shutting down cleanly");
    return 0;
}
