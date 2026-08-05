// Colour type plus the Lattice palette, tuned against Live's dark theme.
#pragma once
#include "../core/common.h"

namespace lat {

struct Col {
    f32 r = 0, g = 0, b = 0, a = 1;
    constexpr Col() = default;
    constexpr Col(f32 R, f32 G, f32 B, f32 A = 1.f) : r(R), g(G), b(B), a(A) {}
    Col alpha(f32 A) const { return Col(r, g, b, A); }
    Col scale(f32 s) const { return Col(r * s, g * s, b * s, a); }
    Col mix(const Col& o, f32 t) const {
        return Col(lerpf(r, o.r, t), lerpf(g, o.g, t), lerpf(b, o.b, t), lerpf(a, o.a, t));
    }
    u32 packed() const {
        return ((u32)(clampv(r, 0.f, 1.f) * 255) << 16) |
               ((u32)(clampv(g, 0.f, 1.f) * 255) << 8) |
                (u32)(clampv(b, 0.f, 1.f) * 255);
    }
};

constexpr Col rgb(u32 hex) {
    return Col(((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, 1.f);
}
constexpr Col rgba(u32 hex, f32 a) {
    return Col(((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, a);
}

namespace pal {
// Surfaces
constexpr Col appBg        = rgb(0x1D1D1D);
constexpr Col panel        = rgb(0x282828);
constexpr Col panelAlt     = rgb(0x2F2F2F);
constexpr Col gridBg       = rgb(0x333333);
constexpr Col slotEmpty    = rgb(0x2A2A2A);
constexpr Col slotHover    = rgb(0x3A3A3A);
constexpr Col divider      = rgb(0x151515);
constexpr Col ridge        = rgb(0x404040);

// Text
constexpr Col text         = rgb(0xD6D6D6);
constexpr Col textDim      = rgb(0x8A8A8A);
constexpr Col textFaint    = rgb(0x5E5E5E);
constexpr Col textOnClip   = rgb(0x1A1A1A);

// Accents
constexpr Col accent       = rgb(0xFF764D);   // selection / focus
constexpr Col playGreen    = rgb(0x59D64B);
constexpr Col recRed       = rgb(0xFF3B30);
constexpr Col armRed       = rgb(0xC8462F);
constexpr Col soloBlue     = rgb(0x4FA3E3);
constexpr Col meterGreen   = rgb(0x62D84E);
constexpr Col meterAmber   = rgb(0xE8C33C);
constexpr Col meterRed     = rgb(0xE8483C);

// The eight-colour clip strip Live cycles through for new tracks.
constexpr Col clipColors[] = {
    rgb(0xFF94A6), rgb(0xFFA529), rgb(0xCC9B54), rgb(0xF7F47C),
    rgb(0xBFFB00), rgb(0x1AFF2F), rgb(0x25FFA8), rgb(0x5CFFE8),
    rgb(0x8BC5FF), rgb(0x5480E4), rgb(0x92A7FF), rgb(0xD86CE4),
    rgb(0xE553A0), rgb(0xFFFFFF), rgb(0xFF3636), rgb(0xF66C03),
};
constexpr int clipColorCount = 16;
} // namespace pal

} // namespace lat
