// FreeType glyph atlas. One texture per size, ASCII plus a few UI symbols.
#pragma once
#include "../core/common.h"
#include <vector>

namespace lat {

struct Glyph {
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    f32 w = 0, h = 0;          // bitmap size in px
    f32 bearingX = 0, bearingY = 0;
    f32 advance = 0;
    bool valid = false;
};

class Font {
public:
    ~Font();
    bool load(const char* path, int pixelSize);
    void destroy();

    const Glyph& glyph(u32 cp) const;
    f32  measure(const char* s, int len = -1) const;
    f32  height()   const { return height_; }
    f32  ascent()   const { return ascent_; }
    f32  descent()  const { return descent_; }
    unsigned tex()  const { return tex_; }
    int  size()     const { return pixelSize_; }

    // Truncates with an ellipsis so it fits `maxW`. Returns bytes to draw and
    // whether an ellipsis is needed.
    int  fitLength(const char* s, f32 maxW, bool* ellipsis) const;

private:
    static constexpr u32 kFirst = 32, kLast = 126;
    Glyph glyphs_[kLast - kFirst + 1];
    Glyph invalid_{};
    unsigned tex_ = 0;
    f32 height_ = 0, ascent_ = 0, descent_ = 0;
    int pixelSize_ = 12;
};

// Finds a usable UI font on this system; returns empty string if none.
std::string findSystemFont(bool bold);

} // namespace lat
