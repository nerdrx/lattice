// Native Wayland backend: xdg-shell + EGL, with server-side decorations,
// fractional scaling via wp_fractional_scale_v1 + wp_viewporter, and xkbcommon
// keyboard handling. This is the default path on a Wayland session; the X11
// backend stays as a fallback.
#include "window_backend.h"
#include "../gfx/gl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#if LAT_HAVE_XDG_DECORATION
#include "xdg-decoration-unstable-v1-client-protocol.h"
#endif
#if LAT_HAVE_FRACTIONAL_SCALE
#include "fractional-scale-v1-client-protocol.h"
#endif
#if LAT_HAVE_VIEWPORTER
#include "viewporter-client-protocol.h"
#endif

#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace lat {
namespace {

int mapSym(xkb_keysym_t ks) {
    switch (ks) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return KeyEnter;
    case XKB_KEY_Escape:      return KeyEscape;
    case XKB_KEY_Tab:         return KeyTab;
    case XKB_KEY_BackSpace:   return KeyBackspace;
    case XKB_KEY_Delete:      return KeyDelete;
    case XKB_KEY_Left:        return KeyLeft;
    case XKB_KEY_Right:       return KeyRight;
    case XKB_KEY_Up:          return KeyUp;
    case XKB_KEY_Down:        return KeyDown;
    case XKB_KEY_Home:        return KeyHome;
    case XKB_KEY_End:         return KeyEnd;
    case XKB_KEY_Page_Up:     return KeyPageUp;
    case XKB_KEY_Page_Down:   return KeyPageDown;
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:     return KeyShift;
    case XKB_KEY_Control_L: case XKB_KEY_Control_R: return KeyCtrl;
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:         return KeyAlt;
    case XKB_KEY_Super_L: case XKB_KEY_Super_R:     return KeySuper;
    case XKB_KEY_F1: return KeyF1;   case XKB_KEY_F2: return KeyF2;
    case XKB_KEY_F3: return KeyF3;   case XKB_KEY_F4: return KeyF4;
    case XKB_KEY_F5: return KeyF5;   case XKB_KEY_F6: return KeyF6;
    case XKB_KEY_F7: return KeyF7;   case XKB_KEY_F8: return KeyF8;
    case XKB_KEY_F9: return KeyF9;   case XKB_KEY_F10: return KeyF10;
    case XKB_KEY_F11: return KeyF11; case XKB_KEY_F12: return KeyF12;
    case XKB_KEY_space: return ' ';
    default: break;
    }
    if (ks >= 32 && ks <= 126) {
        if (ks >= 'A' && ks <= 'Z') return (int)(ks - 'A' + 'a');
        return (int)ks;
    }
    return KeyNone;
}

class WaylandBackend final : public IWindowBackend {
public:
    bool create(const char* title, int w, int h, Input* in) override;
    void destroy() override;
    bool pump() override;
    void swap() override;
    void setCursor(Cursor c) override;
    void setTitle(const char* t) override {
        if (toplevel_) { xdg_toplevel_set_title(toplevel_, t); wl_display_flush(dpy_); }
    }
    int  width()  const override { return w_; }
    int  height() const override { return h_; }
    f32  dpiScale() const override { return (f32)scale_; }
    const char* name() const override { return "Wayland/EGL"; }

    // --- registry ---------------------------------------------------------
    static void regGlobal(void* d, wl_registry* r, u32 id, const char* iface, u32 ver);
    static void regRemove(void*, wl_registry*, u32) {}

    // --- shell ------------------------------------------------------------
    static void wmPing(void*, xdg_wm_base* b, u32 serial) { xdg_wm_base_pong(b, serial); }
    static void surfConfigure(void* d, xdg_surface* s, u32 serial);
    static void topConfigure(void* d, xdg_toplevel*, i32 w, i32 h, wl_array* states);
    static void topClose(void* d, xdg_toplevel*);

    // --- seat -------------------------------------------------------------
    static void seatCaps(void* d, wl_seat* s, u32 caps);
    static void ptrEnter(void* d, wl_pointer*, u32 serial, wl_surface*, wl_fixed_t x, wl_fixed_t y);
    static void ptrLeave(void* d, wl_pointer*, u32, wl_surface*);
    static void ptrMotion(void* d, wl_pointer*, u32, wl_fixed_t x, wl_fixed_t y);
    static void ptrButton(void* d, wl_pointer*, u32 serial, u32, u32 button, u32 state);
    static void ptrAxis(void* d, wl_pointer*, u32, u32 axis, wl_fixed_t value);
    static void ptrAxis120(void* d, wl_pointer*, u32 axis, i32 v120);
    static void kbKeymap(void* d, wl_keyboard*, u32 fmt, i32 fd, u32 size);
    static void kbKey(void* d, wl_keyboard*, u32 serial, u32, u32 key, u32 state);
    static void kbMods(void* d, wl_keyboard*, u32, u32 dep, u32 lat, u32 lock, u32 group);
    static void kbRepeatInfo(void* d, wl_keyboard*, i32 rate, i32 delay);
    static void kbLeave(void* d, wl_keyboard*, u32, wl_surface*);

#if LAT_HAVE_FRACTIONAL_SCALE
    static void fracScaleEv(void* d, wp_fractional_scale_v1*, u32 scale120);
#endif
    static void surfPreferredScale(void* d, wl_surface*, i32 factor);

private:
    void applySize();
    void handleKey(u32 key, bool down, bool isRepeat);
    void loadCursorTheme();

    // Wayland objects
    wl_display*    dpy_    = nullptr;
    wl_registry*   reg_    = nullptr;
    wl_compositor* comp_   = nullptr;
    wl_shm*        shm_    = nullptr;
    wl_seat*       seat_   = nullptr;
    wl_pointer*    ptr_    = nullptr;
    wl_keyboard*   kbd_    = nullptr;
    wl_surface*    surf_   = nullptr;
    xdg_wm_base*   wmBase_ = nullptr;
    xdg_surface*   xsurf_  = nullptr;
    xdg_toplevel*  toplevel_ = nullptr;
#if LAT_HAVE_XDG_DECORATION
    zxdg_decoration_manager_v1*   decoMgr_ = nullptr;
    zxdg_toplevel_decoration_v1*  deco_    = nullptr;
#endif
#if LAT_HAVE_FRACTIONAL_SCALE
    wp_fractional_scale_manager_v1* fracMgr_ = nullptr;
    wp_fractional_scale_v1*         fracScale_ = nullptr;
#endif
#if LAT_HAVE_VIEWPORTER
    wp_viewporter* vpMgr_ = nullptr;
    wp_viewport*   viewport_ = nullptr;
#endif

    // Cursor
    wl_cursor_theme* cursorTheme_ = nullptr;
    wl_surface*      cursorSurf_  = nullptr;
    Cursor           curCursor_   = Cursor::Arrow;
    u32              enterSerial_ = 0;
    int              cursorSize_  = 24;

    // EGL
    EGLDisplay egl_    = EGL_NO_DISPLAY;
    EGLContext eglCtx_ = EGL_NO_CONTEXT;
    EGLSurface eglSurf_= EGL_NO_SURFACE;
    wl_egl_window* eglWin_ = nullptr;

    // xkb
    xkb_context* xkbCtx_ = nullptr;
    xkb_keymap*  xkbMap_ = nullptr;
    xkb_state*   xkbState_ = nullptr;

    // key repeat
    i32 repeatRate_ = 25, repeatDelay_ = 400;
    u32 repeatKey_ = 0;
    bool repeating_ = false;
    std::chrono::steady_clock::time_point repeatNext_{};

    Input* in_ = nullptr;
    // High-resolution scroll and the legacy axis event both arrive for the same
    // gesture; count v120 when the compositor sends it and ignore the legacy one.
    bool gotV120_ = false;
    bool closed_ = false;
    bool configured_ = false;
    bool sizeDirty_ = false;
    int  logicalW_ = 1280, logicalH_ = 800;
    int  pendingW_ = 0, pendingH_ = 0;
    int  w_ = 0, h_ = 0;
    f64  scale_ = 1.0;
    f64  pendingScale_ = 1.0;
    f32  lastX_ = 0, lastY_ = 0;
    bool haveLast_ = false;
    std::chrono::steady_clock::time_point lastClick_{};
    int  lastClickBtn_ = -1;
};

// --- listener tables --------------------------------------------------------
const wl_registry_listener kRegistryL = {
    .global = &WaylandBackend::regGlobal,
    .global_remove = &WaylandBackend::regRemove,
};
const xdg_wm_base_listener kWmBaseL = { .ping = &WaylandBackend::wmPing };
const xdg_surface_listener kXSurfL  = { .configure = &WaylandBackend::surfConfigure };
const xdg_toplevel_listener kTopL = {
    .configure = &WaylandBackend::topConfigure,
    .close = &WaylandBackend::topClose,
    .configure_bounds = [](void*, xdg_toplevel*, i32, i32) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};
const wl_seat_listener kSeatL = {
    .capabilities = &WaylandBackend::seatCaps,
    .name = [](void*, wl_seat*, const char*) {},
};
const wl_pointer_listener kPtrL = {
    .enter = &WaylandBackend::ptrEnter,
    .leave = &WaylandBackend::ptrLeave,
    .motion = &WaylandBackend::ptrMotion,
    .button = &WaylandBackend::ptrButton,
    .axis = &WaylandBackend::ptrAxis,
    .frame = [](void*, wl_pointer*) {},
    .axis_source = [](void*, wl_pointer*, u32) {},
    .axis_stop = [](void*, wl_pointer*, u32, u32) {},
    .axis_discrete = [](void*, wl_pointer*, u32, i32) {},
    .axis_value120 = &WaylandBackend::ptrAxis120,
    .axis_relative_direction = [](void*, wl_pointer*, u32, u32) {},
};
const wl_keyboard_listener kKbdL = {
    .keymap = &WaylandBackend::kbKeymap,
    .enter = [](void*, wl_keyboard*, u32, wl_surface*, wl_array*) {},
    .leave = &WaylandBackend::kbLeave,
    .key = &WaylandBackend::kbKey,
    .modifiers = &WaylandBackend::kbMods,
    .repeat_info = &WaylandBackend::kbRepeatInfo,
};
const wl_surface_listener kSurfL = {
    .enter = [](void*, wl_surface*, wl_output*) {},
    .leave = [](void*, wl_surface*, wl_output*) {},
    .preferred_buffer_scale = &WaylandBackend::surfPreferredScale,
    .preferred_buffer_transform = [](void*, wl_surface*, u32) {},
};
#if LAT_HAVE_FRACTIONAL_SCALE
const wp_fractional_scale_v1_listener kFracL = {
    .preferred_scale = &WaylandBackend::fracScaleEv,
};
#endif
#if LAT_HAVE_XDG_DECORATION
const zxdg_toplevel_decoration_v1_listener kDecoL = {
    .configure = [](void*, zxdg_toplevel_decoration_v1*, u32) {},
};
#endif

// --- registry ---------------------------------------------------------------
void WaylandBackend::regGlobal(void* d, wl_registry* r, u32 id, const char* iface, u32 ver) {
    auto* s = (WaylandBackend*)d;
    auto is = [&](const char* n) { return std::strcmp(iface, n) == 0; };

    if (is(wl_compositor_interface.name)) {
        s->comp_ = (wl_compositor*)wl_registry_bind(r, id, &wl_compositor_interface, std::min(ver, 6u));
    } else if (is(wl_shm_interface.name)) {
        s->shm_ = (wl_shm*)wl_registry_bind(r, id, &wl_shm_interface, 1);
    } else if (is(xdg_wm_base_interface.name)) {
        s->wmBase_ = (xdg_wm_base*)wl_registry_bind(r, id, &xdg_wm_base_interface, std::min(ver, 5u));
        xdg_wm_base_add_listener(s->wmBase_, &kWmBaseL, s);
    } else if (is(wl_seat_interface.name)) {
        s->seat_ = (wl_seat*)wl_registry_bind(r, id, &wl_seat_interface, std::min(ver, 8u));
        wl_seat_add_listener(s->seat_, &kSeatL, s);
    }
#if LAT_HAVE_XDG_DECORATION
    else if (is(zxdg_decoration_manager_v1_interface.name)) {
        s->decoMgr_ = (zxdg_decoration_manager_v1*)wl_registry_bind(
            r, id, &zxdg_decoration_manager_v1_interface, 1);
    }
#endif
#if LAT_HAVE_FRACTIONAL_SCALE
    else if (is(wp_fractional_scale_manager_v1_interface.name)) {
        s->fracMgr_ = (wp_fractional_scale_manager_v1*)wl_registry_bind(
            r, id, &wp_fractional_scale_manager_v1_interface, 1);
    }
#endif
#if LAT_HAVE_VIEWPORTER
    else if (is(wp_viewporter_interface.name)) {
        s->vpMgr_ = (wp_viewporter*)wl_registry_bind(r, id, &wp_viewporter_interface, 1);
    }
#endif
}

// --- shell ------------------------------------------------------------------
void WaylandBackend::surfConfigure(void* d, xdg_surface* s, u32 serial) {
    auto* self = (WaylandBackend*)d;
    xdg_surface_ack_configure(s, serial);
    self->configured_ = true;
    if (self->pendingW_ > 0 && self->pendingH_ > 0) {
        self->logicalW_ = self->pendingW_;
        self->logicalH_ = self->pendingH_;
        self->pendingW_ = self->pendingH_ = 0;
    }
    self->sizeDirty_ = true;
}

void WaylandBackend::topConfigure(void* d, xdg_toplevel*, i32 w, i32 h, wl_array*) {
    auto* self = (WaylandBackend*)d;
    if (w > 0 && h > 0) { self->pendingW_ = w; self->pendingH_ = h; }
}

void WaylandBackend::topClose(void* d, xdg_toplevel*) {
    ((WaylandBackend*)d)->closed_ = true;
}

void WaylandBackend::surfPreferredScale(void* d, wl_surface*, i32 factor) {
    auto* self = (WaylandBackend*)d;
#if LAT_HAVE_FRACTIONAL_SCALE
    if (self->fracScale_) return;          // fractional scale wins when present
#endif
    if (factor > 0) { self->pendingScale_ = (f64)factor; self->sizeDirty_ = true; }
}

#if LAT_HAVE_FRACTIONAL_SCALE
void WaylandBackend::fracScaleEv(void* d, wp_fractional_scale_v1*, u32 scale120) {
    auto* self = (WaylandBackend*)d;
    if (scale120 == 0) return;
    self->pendingScale_ = (f64)scale120 / 120.0;
    self->sizeDirty_ = true;
}
#endif

// --- seat -------------------------------------------------------------------
void WaylandBackend::seatCaps(void* d, wl_seat* s, u32 caps) {
    auto* self = (WaylandBackend*)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->ptr_) {
        self->ptr_ = wl_seat_get_pointer(s);
        wl_pointer_add_listener(self->ptr_, &kPtrL, self);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->kbd_) {
        self->kbd_ = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(self->kbd_, &kKbdL, self);
    }
}

void WaylandBackend::ptrEnter(void* d, wl_pointer*, u32 serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    auto* self = (WaylandBackend*)d;
    self->enterSerial_ = serial;
    self->in_->mx = (f32)(wl_fixed_to_double(x) * self->scale_);
    self->in_->my = (f32)(wl_fixed_to_double(y) * self->scale_);
    self->haveLast_ = false;
    const Cursor want = self->curCursor_;
    self->curCursor_ = (Cursor)~0u;         // force re-apply on the new enter serial
    self->setCursor(want);
}

void WaylandBackend::ptrLeave(void* d, wl_pointer*, u32, wl_surface*) {
    ((WaylandBackend*)d)->haveLast_ = false;
}

void WaylandBackend::ptrMotion(void* d, wl_pointer*, u32, wl_fixed_t x, wl_fixed_t y) {
    auto* self = (WaylandBackend*)d;
    const f32 px = (f32)(wl_fixed_to_double(x) * self->scale_);
    const f32 py = (f32)(wl_fixed_to_double(y) * self->scale_);
    if (self->haveLast_) { self->in_->dx += px - self->lastX_; self->in_->dy += py - self->lastY_; }
    self->lastX_ = px; self->lastY_ = py; self->haveLast_ = true;
    self->in_->mx = px; self->in_->my = py;
}

void WaylandBackend::ptrButton(void* d, wl_pointer*, u32 serial, u32, u32 button, u32 state) {
    auto* self = (WaylandBackend*)d;
    // linux/input-event-codes.h: BTN_LEFT 0x110, BTN_RIGHT 0x111, BTN_MIDDLE 0x112
    const int idx = (button == 0x110) ? 0 : (button == 0x112) ? 1 : (button == 0x111) ? 2 : -1;
    if (idx < 0) return;
    self->enterSerial_ = serial;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        self->in_->down[idx] = true;
        self->in_->pressed[idx] = true;
        const auto now = std::chrono::steady_clock::now();
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - self->lastClick_).count();
        if (idx == self->lastClickBtn_ && gap < 400) self->in_->dblClick = true;
        self->lastClick_ = now;
        self->lastClickBtn_ = idx;
    } else {
        self->in_->down[idx] = false;
        self->in_->released[idx] = true;
    }
}

void WaylandBackend::ptrAxis(void* d, wl_pointer*, u32, u32 axis, wl_fixed_t value) {
    auto* self = (WaylandBackend*)d;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    if (self->gotV120_) return;             // high-res events already counted
    self->in_->wheel += (f32)(-wl_fixed_to_double(value) / 10.0);
}

void WaylandBackend::ptrAxis120(void* d, wl_pointer*, u32 axis, i32 v120) {
    auto* self = (WaylandBackend*)d;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    self->gotV120_ = true;
    self->in_->wheel += (f32)(-v120) / 120.f;
}

void WaylandBackend::kbKeymap(void* d, wl_keyboard*, u32 fmt, i32 fd, u32 size) {
    auto* self = (WaylandBackend*)d;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    if (self->xkbState_) { xkb_state_unref(self->xkbState_); self->xkbState_ = nullptr; }
    if (self->xkbMap_)   { xkb_keymap_unref(self->xkbMap_);  self->xkbMap_ = nullptr; }
    self->xkbMap_ = xkb_keymap_new_from_string(self->xkbCtx_, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (self->xkbMap_) self->xkbState_ = xkb_state_new(self->xkbMap_);
}

void WaylandBackend::kbLeave(void* d, wl_keyboard*, u32, wl_surface*) {
    auto* self = (WaylandBackend*)d;
    std::memset(self->in_->keyDown, 0, sizeof self->in_->keyDown);
    std::memset(self->in_->scanDown, 0, sizeof self->in_->scanDown);
    self->in_->mods = 0;
    self->repeating_ = false;
}

void WaylandBackend::handleKey(u32 key, bool down, bool isRepeat) {
    if (!xkbState_) return;
    // `key` IS the evdev scancode — Wayland delivers it raw, before any
    // layout applies. That is exactly what Input::scanDown wants.
    if (key < 256 && !isRepeat) in_->scanDown[key] = down;
    const xkb_keycode_t kc = key + 8;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState_, kc);
    const int k = mapSym(sym);
    if (k > 0 && k < KeyCount) {
        in_->keyDown[k] = down;
        if (down) in_->keyPressed[k] = true;
    }
    if (down) {
        char buf[64];
        const int n = xkb_state_key_get_utf8(xkbState_, kc, buf, sizeof buf);
        if (n > 0 && (u8)buf[0] >= 32 && (u8)buf[0] != 127) in_->textInput.append(buf, (size_t)n);
    }
    if (!isRepeat) {
        if (down && repeatRate_ > 0 && xkbMap_ && xkb_keymap_key_repeats(xkbMap_, kc)) {
            repeatKey_ = key;
            repeating_ = true;
            repeatNext_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(repeatDelay_);
        } else if (!down && repeatKey_ == key) {
            repeating_ = false;
        }
    }
}

void WaylandBackend::kbKey(void* d, wl_keyboard*, u32, u32, u32 key, u32 state) {
    ((WaylandBackend*)d)->handleKey(key, state == WL_KEYBOARD_KEY_STATE_PRESSED, false);
}

void WaylandBackend::kbMods(void* d, wl_keyboard*, u32, u32 dep, u32 lat_, u32 lock, u32 group) {
    auto* self = (WaylandBackend*)d;
    if (!self->xkbState_) return;
    xkb_state_update_mask(self->xkbState_, dep, lat_, lock, 0, 0, group);
    u32 m = 0;
    auto on = [&](const char* n) {
        return xkb_state_mod_name_is_active(self->xkbState_, n, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    if (on(XKB_MOD_NAME_SHIFT)) m |= ModShift;
    if (on(XKB_MOD_NAME_CTRL))  m |= ModCtrl;
    if (on(XKB_MOD_NAME_ALT))   m |= ModAlt;
    if (on(XKB_MOD_NAME_LOGO))  m |= ModSuper;
    self->in_->mods = m;
}

void WaylandBackend::kbRepeatInfo(void* d, wl_keyboard*, i32 rate, i32 delay) {
    auto* self = (WaylandBackend*)d;
    self->repeatRate_ = rate;
    self->repeatDelay_ = delay;
}

// --- cursors ----------------------------------------------------------------
void WaylandBackend::loadCursorTheme() {
    if (!shm_) return;
    const char* themeName = std::getenv("XCURSOR_THEME");
    if (const char* sz = std::getenv("XCURSOR_SIZE")) {
        const int v = std::atoi(sz);
        if (v > 0) cursorSize_ = v;
    }
    const int scaled = (int)std::lround(cursorSize_ * scale_);
    if (cursorTheme_) wl_cursor_theme_destroy(cursorTheme_);
    cursorTheme_ = wl_cursor_theme_load(themeName, scaled, shm_);
    if (!cursorSurf_ && comp_) cursorSurf_ = wl_compositor_create_surface(comp_);
}

void WaylandBackend::setCursor(Cursor c) {
    if (c == curCursor_) return;
    curCursor_ = c;
    if (!ptr_ || !cursorTheme_ || !cursorSurf_) return;

    static const char* names[6][3] = {
        {"left_ptr", "default", "arrow"},
        {"hand2", "pointer", "hand"},
        {"sb_h_double_arrow", "ew-resize", "col-resize"},
        {"sb_v_double_arrow", "ns-resize", "row-resize"},
        {"xterm", "text", "ibeam"},
        {"fleur", "move", "all-scroll"},
    };
    wl_cursor* cur = nullptr;
    for (int i = 0; i < 3 && !cur; ++i)
        cur = wl_cursor_theme_get_cursor(cursorTheme_, names[(int)c][i]);
    if (!cur || cur->image_count == 0) return;

    wl_cursor_image* img = cur->images[0];
    wl_buffer* buf = wl_cursor_image_get_buffer(img);
    if (!buf) return;

    const int bs = std::max(1, (int)std::lround(scale_));
    wl_surface_set_buffer_scale(cursorSurf_, bs);
    wl_surface_attach(cursorSurf_, buf, 0, 0);
    wl_surface_damage_buffer(cursorSurf_, 0, 0, (i32)img->width, (i32)img->height);
    wl_surface_commit(cursorSurf_);
    wl_pointer_set_cursor(ptr_, enterSerial_, cursorSurf_,
                          (i32)(img->hotspot_x / bs), (i32)(img->hotspot_y / bs));
}

// --- lifecycle --------------------------------------------------------------
void WaylandBackend::applySize() {
    scale_ = pendingScale_;
    const int pw = std::max(1, (int)std::lround(logicalW_ * scale_));
    const int ph = std::max(1, (int)std::lround(logicalH_ * scale_));
    if (eglWin_) wl_egl_window_resize(eglWin_, pw, ph, 0, 0);

#if LAT_HAVE_VIEWPORTER
    if (viewport_) {
        // Buffer is device pixels; the viewport maps it onto the logical size,
        // which is what makes non-integer scales land exactly.
        wp_viewport_set_destination(viewport_, logicalW_, logicalH_);
        wl_surface_set_buffer_scale(surf_, 1);
    } else
#endif
    {
        wl_surface_set_buffer_scale(surf_, std::max(1, (int)std::lround(scale_)));
    }
    w_ = pw; h_ = ph;
    sizeDirty_ = false;
}

bool WaylandBackend::create(const char* title, int w, int h, Input* in) {
    in_ = in;
    logicalW_ = w; logicalH_ = h;

    dpy_ = wl_display_connect(nullptr);
    if (!dpy_) { LOGW("no Wayland display"); return false; }

    reg_ = wl_display_get_registry(dpy_);
    wl_registry_add_listener(reg_, &kRegistryL, this);
    wl_display_roundtrip(dpy_);          // globals
    wl_display_roundtrip(dpy_);          // seat capabilities

    if (!comp_ || !wmBase_) { LOGE("compositor lacks wl_compositor/xdg_wm_base"); return false; }

    xkbCtx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    surf_ = wl_compositor_create_surface(comp_);
    wl_surface_add_listener(surf_, &kSurfL, this);
    xsurf_ = xdg_wm_base_get_xdg_surface(wmBase_, surf_);
    xdg_surface_add_listener(xsurf_, &kXSurfL, this);
    toplevel_ = xdg_surface_get_toplevel(xsurf_);
    xdg_toplevel_add_listener(toplevel_, &kTopL, this);
    xdg_toplevel_set_title(toplevel_, title);
    xdg_toplevel_set_app_id(toplevel_, "org.nxtakt.NxTakt");
    xdg_toplevel_set_min_size(toplevel_, 900, 560);

#if LAT_HAVE_XDG_DECORATION
    if (decoMgr_) {
        deco_ = zxdg_decoration_manager_v1_get_toplevel_decoration(decoMgr_, toplevel_);
        zxdg_toplevel_decoration_v1_add_listener(deco_, &kDecoL, this);
        zxdg_toplevel_decoration_v1_set_mode(deco_, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
#endif
#if LAT_HAVE_FRACTIONAL_SCALE
    if (fracMgr_) {
        fracScale_ = wp_fractional_scale_manager_v1_get_fractional_scale(fracMgr_, surf_);
        wp_fractional_scale_v1_add_listener(fracScale_, &kFracL, this);
    }
#endif
#if LAT_HAVE_VIEWPORTER
    if (vpMgr_) viewport_ = wp_viewporter_get_viewport(vpMgr_, surf_);
#endif

    // The surface must be committed with no buffer, then wait for the first
    // configure before anything may be attached.
    wl_surface_commit(surf_);
    while (!configured_ && wl_display_dispatch(dpy_) != -1) {}

    // --- EGL ---
    if (auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
            eglGetProcAddress("eglGetPlatformDisplayEXT")) {
        egl_ = getPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, dpy_, nullptr);
    }
    if (egl_ == EGL_NO_DISPLAY) egl_ = eglGetDisplay((EGLNativeDisplayType)dpy_);
    if (egl_ == EGL_NO_DISPLAY) { LOGE("eglGetDisplay failed"); return false; }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl_, &major, &minor)) { LOGE("eglInitialize failed"); return false; }
    if (!eglBindAPI(EGL_OPENGL_API)) { LOGE("no desktop GL via EGL"); return false; }

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLConfig cfg = nullptr;
    EGLint nCfg = 0;
    if (!eglChooseConfig(egl_, cfgAttribs, &cfg, 1, &nCfg) || nCfg == 0) {
        LOGE("no suitable EGL config"); return false;
    }

    const EGLint ctxAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    eglCtx_ = eglCreateContext(egl_, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (eglCtx_ == EGL_NO_CONTEXT) { LOGE("cannot create an OpenGL 3.3 core context"); return false; }

    applySize();
    eglWin_ = wl_egl_window_create(surf_, w_, h_);
    if (!eglWin_) { LOGE("wl_egl_window_create failed"); return false; }
    eglSurf_ = eglCreateWindowSurface(egl_, cfg, (EGLNativeWindowType)eglWin_, nullptr);
    if (eglSurf_ == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return false; }
    if (!eglMakeCurrent(egl_, eglSurf_, eglSurf_, eglCtx_)) { LOGE("eglMakeCurrent failed"); return false; }
    eglSwapInterval(egl_, 1);

    loadCursorTheme();
    applySize();

    LOGI("Wayland window %dx%d logical, %dx%d px, scale %.3f%s%s",
         logicalW_, logicalH_, w_, h_, scale_,
#if LAT_HAVE_FRACTIONAL_SCALE
         fracScale_ ? " [fractional]" : "",
#else
         "",
#endif
#if LAT_HAVE_VIEWPORTER
         viewport_ ? " [viewport]" : "");
#else
         "");
#endif
    return true;
}

void WaylandBackend::destroy() {
    if (egl_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurf_ != EGL_NO_SURFACE) eglDestroySurface(egl_, eglSurf_);
        if (eglCtx_ != EGL_NO_CONTEXT) eglDestroyContext(egl_, eglCtx_);
        eglTerminate(egl_);
    }
    if (eglWin_) wl_egl_window_destroy(eglWin_);
    if (cursorTheme_) wl_cursor_theme_destroy(cursorTheme_);
    if (cursorSurf_) wl_surface_destroy(cursorSurf_);
    if (xkbState_) xkb_state_unref(xkbState_);
    if (xkbMap_) xkb_keymap_unref(xkbMap_);
    if (xkbCtx_) xkb_context_unref(xkbCtx_);
    if (toplevel_) xdg_toplevel_destroy(toplevel_);
    if (xsurf_) xdg_surface_destroy(xsurf_);
    if (surf_) wl_surface_destroy(surf_);
    if (dpy_) wl_display_disconnect(dpy_);
    dpy_ = nullptr;
}

bool WaylandBackend::pump() {
    if (!dpy_) return false;
    in_->newFrame();
    gotV120_ = false;

    // Non-blocking dispatch: we drive the frame rate from eglSwapBuffers.
    while (wl_display_prepare_read(dpy_) != 0) wl_display_dispatch_pending(dpy_);
    wl_display_flush(dpy_);
    pollfd pfd{wl_display_get_fd(dpy_), POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) wl_display_read_events(dpy_);
    else wl_display_cancel_read(dpy_);
    if (wl_display_dispatch_pending(dpy_) < 0) return false;

    if (repeating_ && repeatRate_ > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto period = std::chrono::milliseconds(std::max(1, 1000 / repeatRate_));
        while (repeatNext_ <= now) {
            handleKey(repeatKey_, true, true);
            repeatNext_ += period;
        }
    }

    if (sizeDirty_) {
        applySize();
        loadCursorTheme();
    }
    return !closed_;
}

void WaylandBackend::swap() {
    if (egl_ != EGL_NO_DISPLAY && eglSurf_ != EGL_NO_SURFACE) eglSwapBuffers(egl_, eglSurf_);
}

} // namespace

IWindowBackend* createWaylandBackend() { return new WaylandBackend(); }

} // namespace lat
