#include "font.h"
#include "gl.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstring>
#include <unistd.h>

namespace lat {

static FT_Library g_ft = nullptr;
static bool ensureFT() {
    if (g_ft) return true;
    if (FT_Init_FreeType(&g_ft)) { LOGE("FreeType init failed"); g_ft = nullptr; return false; }
    return true;
}

std::string findSystemFont(bool bold) {
    static const char* regular[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        nullptr};
    static const char* boldFonts[] = {
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        nullptr};
    for (const char** p = bold ? boldFonts : regular; *p; ++p)
        if (access(*p, R_OK) == 0) return *p;
    // Last resort: whatever the regular list can offer.
    if (bold) for (const char** p = regular; *p; ++p)
        if (access(*p, R_OK) == 0) return *p;
    return {};
}

Font::~Font() { destroy(); }

void Font::destroy() {
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
}

bool Font::load(const char* path, int pixelSize) {
    if (!ensureFT()) return false;
    FT_Face face = nullptr;
    if (FT_New_Face(g_ft, path, 0, &face)) { LOGE("cannot open font %s", path); return false; }
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixelSize);
    pixelSize_ = pixelSize;
    ascent_  =  face->size->metrics.ascender  / 64.f;
    descent_ = -face->size->metrics.descender / 64.f;
    height_  =  face->size->metrics.height    / 64.f;

    // Lay glyphs out in rows; ASCII at UI sizes fits comfortably in 512px.
    const int atlasW = 512;
    int penX = 1, penY = 1, rowH = 0, needH = 1;
    for (u32 cp = kFirst; cp <= kLast; ++cp) {
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) continue;
        const int gw = (int)face->glyph->bitmap.width, gh = (int)face->glyph->bitmap.rows;
        if (penX + gw + 1 > atlasW) { penX = 1; penY += rowH + 1; rowH = 0; }
        penX += gw + 1;
        if (gh > rowH) rowH = gh;
        needH = penY + rowH + 1;
    }
    int atlasH = 1; while (atlasH < needH) atlasH <<= 1;

    std::vector<u8> pix((size_t)atlasW * atlasH, 0);
    penX = 1; penY = 1; rowH = 0;
    for (u32 cp = kFirst; cp <= kLast; ++cp) {
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) continue;
        FT_GlyphSlot g = face->glyph;
        const int gw = (int)g->bitmap.width, gh = (int)g->bitmap.rows;
        if (penX + gw + 1 > atlasW) { penX = 1; penY += rowH + 1; rowH = 0; }
        for (int y = 0; y < gh; ++y)
            std::memcpy(&pix[(size_t)(penY + y) * atlasW + penX], g->bitmap.buffer + (size_t)y * g->bitmap.pitch, (size_t)gw);

        Glyph& gl = glyphs_[cp - kFirst];
        gl.u0 = (f32)penX / atlasW;
        gl.v0 = (f32)penY / atlasH;
        gl.u1 = (f32)(penX + gw) / atlasW;
        gl.v1 = (f32)(penY + gh) / atlasH;
        gl.w = (f32)gw; gl.h = (f32)gh;
        gl.bearingX = (f32)g->bitmap_left;
        gl.bearingY = (f32)g->bitmap_top;
        gl.advance  = g->advance.x / 64.f;
        gl.valid = true;

        penX += gw + 1;
        if (gh > rowH) rowH = gh;
    }
    FT_Done_Face(face);

    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlasW, atlasH, 0, GL_RED, GL_UNSIGNED_BYTE, pix.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

const Glyph& Font::glyph(u32 cp) const {
    if (cp < kFirst || cp > kLast) return invalid_;
    return glyphs_[cp - kFirst];
}

f32 Font::measure(const char* s, int len) const {
    if (!s) return 0.f;
    f32 w = 0.f;
    for (int i = 0; (len < 0 ? s[i] != 0 : i < len); ++i) w += glyph((u8)s[i]).advance;
    return w;
}

int Font::fitLength(const char* s, f32 maxW, bool* ell) const {
    *ell = false;
    if (!s) return 0;
    const f32 dotW = glyph('.').advance * 3.f;
    f32 w = 0.f;
    int i = 0;
    for (; s[i]; ++i) {
        const f32 aw = glyph((u8)s[i]).advance;
        if (w + aw > maxW) break;
        w += aw;
    }
    if (!s[i]) return i;                       // fits whole
    *ell = true;
    while (i > 0 && w + dotW > maxW) { w -= glyph((u8)s[i - 1]).advance; --i; }
    return i;
}

} // namespace lat
