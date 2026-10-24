// GpuDemo — Direct GPU rendering demo for FreeBSD-X
//
// Demonstrates XGpuSurface bypassing the xkit widget framework entirely.
// This mirrors how a game or GPU-native app would work: connect to xserver,
// get a surface, render via OpenGL ES 2.0, export DMA-buf frames directly.
//
// Pipeline:
//   connect to /tmp/freebsdx.sock
//   → SurfaceCreated handshake
//   → SurfaceReady (gets dims)
//   → XGpuSurface::create (DRM/GBM/EGL setup)
//   → render loop: beginFrame → GLES draw → endFrame (sends DMA-buf via SCM_RIGHTS)

#include "xkit/gpu_surface.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <GLES2/gl2.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>

// ─── shaders ─────────────────────────────────────────────────────────────────

static const char* kVertSrc = R"glsl(
attribute vec2 a_pos;
varying   vec2 v_uv;
void main() {
    v_uv        = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)glsl";

// Animated colour wheel: hue from angle, saturation from radius, time-rotated.
static const char* kFragSrc = R"glsl(
precision mediump float;
varying vec2  v_uv;
uniform float u_time;

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec2  d   = v_uv - 0.5;
    float r   = length(d) * 2.0;
    float hue = fract(atan(d.y, d.x) / (2.0 * 3.14159265) + u_time * 0.1);
    float val = 1.0 - smoothstep(0.88, 1.0, r);
    gl_FragColor = vec4(hsv2rgb(vec3(hue, clamp(r, 0.0, 1.0), val)), val);
}
)glsl";

// ─── GL helpers ──────────────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "[GpuDemo] shader error: " << log << "\n";
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint build_program() {
    GLuint v = compile_shader(GL_VERTEX_SHADER,   kVertSrc);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, kFragSrc);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    return p;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    using namespace fbx;
    using namespace fbx::xkit;
    using namespace fbx::shared;

    // 1. Connect to compositor
    auto sock = std::make_shared<xserver::IpcSocket>("/tmp/freebsdx.sock");
    for (int i = 0; i < 50; ++i) {
        if (sock->connect()) break;
        if (i == 49) { std::cerr << "[GpuDemo] Cannot connect to xserver\n"; return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 2. Announce ourselves
    uint32_t surface_id = static_cast<uint32_t>(getpid()) & 0xFFFF;
    RenderMessage hello{};
    hello.surface_id_ = surface_id;
    hello.status_     = RenderMessageStatus::SurfaceCreated;
    hello.width_      = 1280;
    hello.height_     = 720;
    sock->sendMessage({reinterpret_cast<const uint8_t*>(&hello), sizeof(hello)});

    // 3. Wait for SurfaceReady to learn the logical window size
    uint32_t w = 1280, h = 720;
    while (true) {
        auto res = sock->receiveMessage();
        if (!res) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        const auto* rm = reinterpret_cast<const RenderMessage*>(res->first.data());
        if (rm->status_ == RenderMessageStatus::SurfaceReady) {
            w = rm->width_; h = rm->height_;
            std::cout << "[GpuDemo] SurfaceReady " << w << "×" << h << "\n";
            break;
        }
    }

    // 4. Create GPU surface (DRM/GBM/EGL)
    auto gpu = XGpuSurface::create(surface_id, w, h, "/dev/dri/renderD128");
    if (!gpu) { std::cerr << "[GpuDemo] XGpuSurface::create failed\n"; return 1; }

    // 5. Build GL resources (context is already current after create())
    gpu->beginFrame();
    GLuint prog = build_program();
    if (!prog) return 1;
    GLint u_time = glGetUniformLocation(prog, "u_time");

    static const float kQuad[] = {
        -1.f,-1.f,  1.f,-1.f,  -1.f, 1.f,
         1.f,-1.f,  1.f, 1.f,  -1.f, 1.f,
    };
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 6. Render loop ~60 FPS
    constexpr auto kFrameBudget = std::chrono::microseconds(16667);
    float t = 0.0f;
    int ipc_fd = sock->fd();

    while (true) {
        auto t0 = std::chrono::steady_clock::now();

        if (!gpu->beginFrame()) break;

        glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniform1f(u_time, t);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        if (!gpu->endFrame(ipc_fd)) break;

        t += 1.0f / 60.0f;

        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < kFrameBudget)
            std::this_thread::sleep_for(kFrameBudget - elapsed);
    }

    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    return 0;
}
