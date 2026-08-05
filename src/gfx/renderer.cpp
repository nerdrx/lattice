#include "renderer.h"
#include "gl.h"
#include <cstring>

namespace lat {

// Mode encoding carried per-vertex:
//   0 = rounded-rect fill (SDF)
//   1 = glyph (atlas red channel as coverage)
//   2 = flat, no SDF (triangles, gradients, diagonal lines)
//   3 = rounded-rect outline (thickness travels in the u slot)
//   4 = full RGBA texture, tinted by vCol. This is the compositing path for
//       anything rendered outside this batcher -- plugin editors, waveform
//       caches, or a future 3D module drawing into its own FBO.
static const char* kVert = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
layout(location=3) in vec4 aLocal;   // lx, ly, halfW, halfH
layout(location=4) in vec2 aParm;    // radius, mode
uniform vec2 uViewport;
out vec2 vUV; out vec4 vCol; out vec4 vLocal; out vec2 vParm;
void main() {
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                    1.0 - aPos.y / uViewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV; vCol = aCol; vLocal = aLocal; vParm = aParm;
}
)";

static const char* kFrag = R"(#version 330 core
in vec2 vUV; in vec4 vCol; in vec4 vLocal; in vec2 vParm;
uniform sampler2D uTex;
out vec4 fragColor;

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    int mode = int(vParm.y + 0.5);
    if (mode == 1) {
        float cov = texture(uTex, vUV).r;
        fragColor = vec4(vCol.rgb, vCol.a * cov);
    } else if (mode == 2) {
        fragColor = vCol;
    } else if (mode == 3) {
        float d = sdRoundRect(vLocal.xy, vLocal.zw, vParm.x);
        float t = vUV.x;
        float ad = abs(d + t * 0.5) - t * 0.5;
        float a = clamp(0.5 - ad, 0.0, 1.0);
        fragColor = vec4(vCol.rgb, vCol.a * a);
    } else if (mode == 4) {
        fragColor = texture(uTex, vUV) * vCol;
    } else {
        float d = sdRoundRect(vLocal.xy, vLocal.zw, vParm.x);
        float a = clamp(0.5 - d, 0.0, 1.0);
        fragColor = vec4(vCol.rgb, vCol.a * a);
    }
    if (fragColor.a <= 0.0) discard;
}
)";

static unsigned compile(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("shader compile failed:\n%s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool Renderer::init() {
    // No loader call here: on Linux libGL exports the core profile directly,
    // and on Windows the Win32 backend resolves the entry points before this
    // runs. Verify we really have a 3.3 context before touching anything else.
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (!ver) { LOGE("no current OpenGL context"); return false; }

    const unsigned vs = compile(GL_VERTEX_SHADER, kVert);
    const unsigned fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    int ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog_, sizeof log, nullptr, log);
        LOGE("shader link failed:\n%s", log);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    uViewport_ = glGetUniformLocation(prog_, "uViewport");
    uTex_      = glGetUniformLocation(prog_, "uTex");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLsizei stride = sizeof(Vtx);
    auto attr = [&](int idx, int n, size_t off) {
        glEnableVertexAttribArray((GLuint)idx);
        glVertexAttribPointer((GLuint)idx, n, GL_FLOAT, GL_FALSE, stride, (void*)off);
    };
    attr(0, 2, offsetof(Vtx, x));
    attr(1, 2, offsetof(Vtx, u));
    attr(2, 4, offsetof(Vtx, r));
    attr(3, 4, offsetof(Vtx, lx));
    attr(4, 2, offsetof(Vtx, rad));
    glBindVertexArray(0);

    verts_.reserve(64 * 1024);
    LOGI("renderer up: %s / %s", (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));
    return true;
}

void Renderer::shutdown() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (prog_) glDeleteProgram(prog_);
    vbo_ = vao_ = prog_ = 0;
}

void Renderer::begin(int w, int h, f32 dpiScale) {
    vw_ = w; vh_ = h; dpi_ = dpiScale;
    drawCalls_ = 0;
    verts_.clear();
    clips_.clear();
    curTex_ = 0;

    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(prog_);
    glUniform2f(uViewport_, (f32)w, (f32)h);
    glUniform1i(uTex_, 0);
}

void Renderer::end() {
    flush();
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
}

void Renderer::flush() {
    if (verts_.empty()) return;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(Vtx)), verts_.data(), GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, curTex_);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts_.size());
    verts_.clear();
    ++drawCalls_;
}

void Renderer::useTexture(unsigned t) {
    if (t != curTex_) { flush(); curTex_ = t; }
}

void Renderer::quad(const Rect& r, f32 rad, f32 mode, const Col& c,
                    f32 u0, f32 v0, f32 u1, f32 v1) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    const bool sdf = (mode < 0.5f) || (mode > 2.5f);
    const f32 pad = sdf ? 1.f : 0.f;
    const f32 hw = r.w * 0.5f, hh = r.h * 0.5f;
    const f32 x0 = r.x - pad, y0 = r.y - pad, x1 = r.right() + pad, y1 = r.bottom() + pad;
    const f32 lx0 = -hw - pad, ly0 = -hh - pad, lx1 = hw + pad, ly1 = hh + pad;
    const f32 rr = clampv(rad, 0.f, std::min(hw, hh));

    const Vtx a{x0, y0, u0, v0, c.r, c.g, c.b, c.a, lx0, ly0, hw, hh, rr, mode};
    const Vtx b{x1, y0, u1, v0, c.r, c.g, c.b, c.a, lx1, ly0, hw, hh, rr, mode};
    const Vtx d{x1, y1, u1, v1, c.r, c.g, c.b, c.a, lx1, ly1, hw, hh, rr, mode};
    const Vtx e{x0, y1, u0, v1, c.r, c.g, c.b, c.a, lx0, ly1, hw, hh, rr, mode};
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(d);
    verts_.push_back(a); verts_.push_back(d); verts_.push_back(e);
}

void Renderer::rect(const Rect& r, const Col& c)                   { useTexture(0); quad(r, 0.f, 0.f, c); }
void Renderer::roundRect(const Rect& r, f32 rad, const Col& c)     { useTexture(0); quad(r, rad, 0.f, c); }
void Renderer::circle(f32 cx, f32 cy, f32 rad, const Col& c) {
    useTexture(0);
    quad({cx - rad, cy - rad, rad * 2, rad * 2}, rad, 0.f, c);
}
void Renderer::roundRectOutline(const Rect& r, f32 rad, f32 th, const Col& c) {
    useTexture(0);
    quad(r, rad, 3.f, c, th, 0.f, th, 0.f);   // thickness rides in u
}

void Renderer::image(const Rect& r, unsigned tex, const Col& tint, bool flipY) {
    useTexture(tex);
    const f32 v0 = flipY ? 1.f : 0.f, v1 = flipY ? 0.f : 1.f;
    quad(r, 0.f, 4.f, tint, 0.f, v0, 1.f, v1);
}

// --- foreign render passes -------------------------------------------------
// A module that draws with its own shaders (a plugin editor, a spectrum view,
// a 3D scene in an FBO) must be fenced off from the UI batch: the pending
// quads have to land first, and the GL state we rely on has to be restored
// afterwards rather than assumed.
void Renderer::beginForeignPass() {
    flush();
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    curTex_ = 0;
}

void Renderer::endForeignPass() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, vw_, vh_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    glUniform2f(uViewport_, (f32)vw_, (f32)vh_);
    glUniform1i(uTex_, 0);
    if (clips_.empty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        const Rect& nr = clips_.back();
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)nr.x, (int)((f32)vh_ - nr.bottom()), (int)std::max(0.f, nr.w), (int)std::max(0.f, nr.h));
    }
}

void Renderer::vgrad(const Rect& r, const Col& top, const Col& bot) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    useTexture(0);
    const f32 x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    auto V = [&](f32 x, f32 y, const Col& c) {
        return Vtx{x, y, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0, 0, 2.f};
    };
    const Vtx a = V(x0, y0, top), b = V(x1, y0, top), d = V(x1, y1, bot), e = V(x0, y1, bot);
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(d);
    verts_.push_back(a); verts_.push_back(d); verts_.push_back(e);
}

void Renderer::triangle(f32 ax, f32 ay, f32 bx, f32 by, f32 cx_, f32 cy_, const Col& c) {
    useTexture(0);
    auto V = [&](f32 x, f32 y) { return Vtx{x, y, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0, 0, 2.f}; };
    verts_.push_back(V(ax, ay)); verts_.push_back(V(bx, by)); verts_.push_back(V(cx_, cy_));
}

void Renderer::line(f32 x0, f32 y0, f32 x1, f32 y1, f32 th, const Col& c) {
    if (std::fabs(y1 - y0) < 0.01f) {                     // horizontal fast path
        rect({std::min(x0, x1), y0 - th * 0.5f, std::fabs(x1 - x0), th}, c);
        return;
    }
    if (std::fabs(x1 - x0) < 0.01f) {                     // vertical fast path
        rect({x0 - th * 0.5f, std::min(y0, y1), th, std::fabs(y1 - y0)}, c);
        return;
    }
    const f32 dx = x1 - x0, dy = y1 - y0;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const f32 nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    useTexture(0);
    auto V = [&](f32 x, f32 y) { return Vtx{x, y, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0, 0, 2.f}; };
    const Vtx a = V(x0 + nx, y0 + ny), b = V(x1 + nx, y1 + ny);
    const Vtx d = V(x1 - nx, y1 - ny), e = V(x0 - nx, y0 - ny);
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(d);
    verts_.push_back(a); verts_.push_back(d); verts_.push_back(e);
}

f32 Renderer::text(const Font& f, f32 x, f32 y, const char* s, const Col& c, int len) {
    if (!s || !*s) return x;
    useTexture(f.tex());
    const f32 baseline = std::round(y + f.ascent());
    f32 pen = std::round(x);
    for (int i = 0; (len < 0 ? s[i] != 0 : i < len); ++i) {
        const Glyph& g = f.glyph((u8)s[i]);
        if (g.valid && g.w > 0.f && g.h > 0.f) {
            const Rect r{pen + g.bearingX, baseline - g.bearingY, g.w, g.h};
            quad(r, 0.f, 1.f, c, g.u0, g.v0, g.u1, g.v1);
        }
        pen += g.advance;
    }
    return pen;
}

f32 Renderer::textIn(const Font& f, const Rect& r, const char* s, const Col& c, Align a, f32 padX) {
    if (!s || !*s) return r.x;
    const f32 avail = r.w - padX * 2.f;
    if (avail <= 1.f) return r.x;
    bool ell = false;
    const int n = f.fitLength(s, avail, &ell);
    const f32 w = f.measure(s, n) + (ell ? f.measure("...") : 0.f);
    f32 x = r.x + padX;
    if (a == Align::Center) x = r.x + (r.w - w) * 0.5f;
    else if (a == Align::Right) x = r.right() - padX - w;
    const f32 y = r.y + (r.h - f.height()) * 0.5f;
    f32 end = text(f, x, y, s, c, n);
    if (ell) end = text(f, end, y, "...", c);
    return end;
}

void Renderer::pushClip(const Rect& r) {
    const Rect cur = currentClip();
    const Rect nr = clips_.empty() ? r : r.intersect(cur);
    clips_.push_back(nr);
    flush();
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)nr.x, (int)((f32)vh_ - nr.bottom()), (int)std::max(0.f, nr.w), (int)std::max(0.f, nr.h));
}

void Renderer::popClip() {
    if (clips_.empty()) return;
    clips_.pop_back();
    flush();
    if (clips_.empty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        const Rect& nr = clips_.back();
        glScissor((int)nr.x, (int)((f32)vh_ - nr.bottom()), (int)std::max(0.f, nr.w), (int)std::max(0.f, nr.h));
    }
}

} // namespace lat
