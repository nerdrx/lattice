// Batched 2D renderer. Everything the UI draws is a quad; rounded corners and
// outlines come from a signed-distance field in the fragment shader, so there
// is exactly one shader and (usually) one draw call per frame.
#pragma once
#include "../core/common.h"
#include "color.h"
#include "font.h"
#include <vector>

namespace lat {

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
    bool contains(f32 px, f32 py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inset(f32 d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect insetXY(f32 dx, f32 dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    f32  right()  const { return x + w; }
    f32  bottom() const { return y + h; }
    f32  cx() const { return x + w * 0.5f; }
    f32  cy() const { return y + h * 0.5f; }
    Rect intersect(const Rect& o) const {
        const f32 x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const f32 x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
};

enum class Align { Left, Center, Right };

class Renderer {
public:
    bool init();
    void shutdown();

    void begin(int w, int h, f32 dpiScale);
    void end();

    // Shapes
    void rect(const Rect& r, const Col& c);
    void roundRect(const Rect& r, f32 radius, const Col& c);
    void roundRectOutline(const Rect& r, f32 radius, f32 thickness, const Col& c);
    void circle(f32 cx, f32 cy, f32 radius, const Col& c);
    void line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, const Col& c);
    void triangle(f32 ax, f32 ay, f32 bx, f32 by, f32 cx_, f32 cy_, const Col& c);
    // Vertical gradient; used for faders and meter bodies.
    void vgrad(const Rect& r, const Col& top, const Col& bottom);
    // Composites an RGBA texture -- plugin editors, cached waveforms, or an
    // offscreen 3D pass. `flipY` for textures produced by an FBO.
    void image(const Rect& r, unsigned tex, const Col& tint = Col(1, 1, 1, 1), bool flipY = false);

    // Fence around drawing that uses its own shaders and GL state. Any module
    // rendering outside this batcher must sit between these two calls.
    void beginForeignPass();
    void endForeignPass();

    // Text. `y` is the top of the line box; the baseline is derived.
    f32  text(const Font& f, f32 x, f32 y, const char* s, const Col& c, int len = -1);
    f32  textIn(const Font& f, const Rect& r, const char* s, const Col& c,
                Align a = Align::Left, f32 padX = 4.f);

    // Scissor stack. Pushing intersects with the current clip.
    void pushClip(const Rect& r);
    void popClip();
    Rect currentClip() const { return clips_.empty() ? Rect{0, 0, (f32)vw_, (f32)vh_} : clips_.back(); }

    int  width()  const { return vw_; }
    int  height() const { return vh_; }
    int  drawCalls() const { return drawCalls_; }

private:
    struct Vtx { f32 x, y, u, v, r, g, b, a, lx, ly, hw, hh, rad, mode; };

    void flush();
    void quad(const Rect& r, f32 rad, f32 mode, const Col& c,
              f32 u0 = 0, f32 v0 = 0, f32 u1 = 0, f32 v1 = 0);
    void useTexture(unsigned t);

    std::vector<Vtx> verts_;
    std::vector<Rect> clips_;
    unsigned vao_ = 0, vbo_ = 0, prog_ = 0, curTex_ = 0;
    int vw_ = 0, vh_ = 0;
    int uViewport_ = -1, uTex_ = -1;
    f32 dpi_ = 1.f;
    int drawCalls_ = 0;
};

} // namespace lat
