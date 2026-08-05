// X11 + GLX backend. Kept as the fallback for X sessions and remote displays;
// on a Wayland session the native backend is preferred.
#include "window_backend.h"
#include "../gfx/gl.h"
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/XKBlib.h>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace lat {
namespace {

int mapKey(KeySym ks) {
    switch (ks) {
    case XK_Return: case XK_KP_Enter: return KeyEnter;
    case XK_Escape:      return KeyEscape;
    case XK_Tab:         return KeyTab;
    case XK_BackSpace:   return KeyBackspace;
    case XK_Delete:      return KeyDelete;
    case XK_Left:        return KeyLeft;
    case XK_Right:       return KeyRight;
    case XK_Up:          return KeyUp;
    case XK_Down:        return KeyDown;
    case XK_Home:        return KeyHome;
    case XK_End:         return KeyEnd;
    case XK_Page_Up:     return KeyPageUp;
    case XK_Page_Down:   return KeyPageDown;
    case XK_Shift_L: case XK_Shift_R:     return KeyShift;
    case XK_Control_L: case XK_Control_R: return KeyCtrl;
    case XK_Alt_L: case XK_Alt_R:         return KeyAlt;
    case XK_Super_L: case XK_Super_R:     return KeySuper;
    case XK_F1: return KeyF1;   case XK_F2: return KeyF2;   case XK_F3: return KeyF3;
    case XK_F4: return KeyF4;   case XK_F5: return KeyF5;   case XK_F6: return KeyF6;
    case XK_F7: return KeyF7;   case XK_F8: return KeyF8;   case XK_F9: return KeyF9;
    case XK_F10: return KeyF10; case XK_F11: return KeyF11; case XK_F12: return KeyF12;
    case XK_space: return ' ';
    default: break;
    }
    if (ks >= 32 && ks <= 126) {
        if (ks >= 'A' && ks <= 'Z') return (int)(ks - 'A' + 'a');
        return (int)ks;
    }
    return KeyNone;
}

f32 detectDpiScale(Display* dpy) {
    if (const char* s = std::getenv("LATTICE_SCALE")) {
        const f32 v = (f32)std::atof(s);
        if (v > 0.4f && v < 5.f) return v;
    }
    if (char* rms = XResourceManagerString(dpy)) {
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db) {
            char* type = nullptr;
            XrmValue val;
            const bool got = XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) && val.addr;
            const f32 dpi = got ? (f32)std::atof(val.addr) : 0.f;
            XrmDestroyDatabase(db);
            if (dpi > 40.f) return clampv(dpi / 96.f, 1.f, 3.f);
        }
    }
    if (const char* s = std::getenv("GDK_SCALE")) {
        const f32 v = (f32)std::atof(s);
        if (v >= 1.f && v < 5.f) return v;
    }
    return 1.f;
}

class X11Backend final : public IWindowBackend {
public:
    bool create(const char* title, int w, int h, Input* in) override;
    void destroy() override;
    bool pump() override;
    void swap() override { if (dpy_) glXSwapBuffers(dpy_, win_); }
    void setCursor(Cursor c) override {
        if (!dpy_ || c == curCursor_) return;
        curCursor_ = c;
        XDefineCursor(dpy_, win_, cursors_[(int)c]);
    }
    void setTitle(const char* t) override { if (dpy_) XStoreName(dpy_, win_, t); }
    int  width()  const override { return w_; }
    int  height() const override { return h_; }
    f32  dpiScale() const override { return dpi_; }
    const char* name() const override { return "X11/GLX"; }

private:
    Display* dpy_ = nullptr;
    ::Window win_ = 0;
    GLXContext ctx_ = nullptr;
    Atom wmDelete_ = 0;
    Colormap cmap_ = 0;
    ::Cursor cursors_[6]{};
    Cursor curCursor_ = Cursor::Arrow;
    Input* in_ = nullptr;
    bool closed_ = false;
    f32 lastX_ = 0, lastY_ = 0;
    bool haveLast_ = false;
    std::chrono::steady_clock::time_point lastClick_{};
    int lastClickBtn_ = -1;
    int w_ = 0, h_ = 0;
    f32 dpi_ = 1.f;
};

bool X11Backend::create(const char* title, int w, int h, Input* in) {
    in_ = in;
    XInitThreads();
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) { LOGW("cannot open X display"); return false; }

    static int visAttribs[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 0, GLX_STENCIL_SIZE, 0,
        GLX_DOUBLEBUFFER, True,
        None
    };
    int nfb = 0;
    GLXFBConfig* fbs = glXChooseFBConfig(dpy_, DefaultScreen(dpy_), visAttribs, &nfb);
    if (!fbs || nfb == 0) { LOGE("no suitable GLX framebuffer config"); return false; }
    GLXFBConfig fb = fbs[0];
    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy_, fb);
    XFree(fbs);
    if (!vi) { LOGE("glXGetVisualFromFBConfig failed"); return false; }

    dpi_ = detectDpiScale(dpy_);
    w = (int)(w * dpi_);
    h = (int)(h * dpi_);

    ::Window root = RootWindow(dpy_, vi->screen);
    cmap_ = XCreateColormap(dpy_, root, vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = cmap_;
    swa.background_pixmap = None;
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     FocusChangeMask | LeaveWindowMask;
    win_ = XCreateWindow(dpy_, root, 0, 0, (unsigned)w, (unsigned)h, 0, vi->depth,
                         InputOutput, vi->visual,
                         CWBorderPixel | CWColormap | CWEventMask, &swa);
    if (!win_) { LOGE("XCreateWindow failed"); XFree(vi); return false; }

    XStoreName(dpy_, win_, title);
    XSizeHints* sh = XAllocSizeHints();
    sh->flags = PMinSize;
    sh->min_width = 900; sh->min_height = 560;
    XSetWMNormalHints(dpy_, win_, sh);
    XFree(sh);

    wmDelete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy_, win_, &wmDelete_, 1);
    XMapWindow(dpy_, win_);

    using CreateCtxFn = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    auto createCtx = (CreateCtxFn)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    if (createCtx) {
        static int ctxAttribs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        ctx_ = createCtx(dpy_, fb, nullptr, True, ctxAttribs);
    }
    if (!ctx_) ctx_ = glXCreateNewContext(dpy_, fb, GLX_RGBA_TYPE, nullptr, True);
    XFree(vi);
    if (!ctx_) { LOGE("cannot create an OpenGL 3.3 context"); return false; }

    glXMakeCurrent(dpy_, win_, ctx_);
    using SwapFn = void (*)(Display*, GLXDrawable, int);
    if (auto si = (SwapFn)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT"))
        si(dpy_, win_, 1);

    Bool supported = False;
    XkbSetDetectableAutoRepeat(dpy_, True, &supported);

    cursors_[(int)Cursor::Arrow]   = XCreateFontCursor(dpy_, XC_left_ptr);
    cursors_[(int)Cursor::Hand]    = XCreateFontCursor(dpy_, XC_hand2);
    cursors_[(int)Cursor::ResizeH] = XCreateFontCursor(dpy_, XC_sb_h_double_arrow);
    cursors_[(int)Cursor::ResizeV] = XCreateFontCursor(dpy_, XC_sb_v_double_arrow);
    cursors_[(int)Cursor::Text]    = XCreateFontCursor(dpy_, XC_xterm);
    cursors_[(int)Cursor::Grab]    = XCreateFontCursor(dpy_, XC_fleur);

    w_ = w; h_ = h;
    LOGI("X11 window %dx%d, scale %.2f", w_, h_, dpi_);
    return true;
}

void X11Backend::destroy() {
    if (!dpy_) return;
    if (ctx_) { glXMakeCurrent(dpy_, None, nullptr); glXDestroyContext(dpy_, ctx_); }
    if (win_) XDestroyWindow(dpy_, win_);
    if (cmap_) XFreeColormap(dpy_, cmap_);
    XCloseDisplay(dpy_);
    dpy_ = nullptr;
}

bool X11Backend::pump() {
    if (!dpy_) return false;
    in_->newFrame();

    while (XPending(dpy_)) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        switch (ev.type) {
        case ConfigureNotify:
            w_ = ev.xconfigure.width;
            h_ = ev.xconfigure.height;
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wmDelete_) closed_ = true;
            break;
        case MotionNotify: {
            const f32 x = (f32)ev.xmotion.x, y = (f32)ev.xmotion.y;
            if (haveLast_) { in_->dx += x - lastX_; in_->dy += y - lastY_; }
            lastX_ = x; lastY_ = y; haveLast_ = true;
            in_->mx = x; in_->my = y;
            break;
        }
        case ButtonPress: {
            const unsigned b = ev.xbutton.button;
            if (b == Button4) { in_->wheel += 1.f; break; }
            if (b == Button5) { in_->wheel -= 1.f; break; }
            if (b == 6 || b == 7) break;
            const int idx = (b == Button1) ? 0 : (b == Button2) ? 1 : (b == Button3) ? 2 : -1;
            if (idx < 0) break;
            in_->down[idx] = true;
            in_->pressed[idx] = true;
            in_->mx = (f32)ev.xbutton.x; in_->my = (f32)ev.xbutton.y;
            const auto now = std::chrono::steady_clock::now();
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClick_).count();
            if (idx == lastClickBtn_ && gap < 400) in_->dblClick = true;
            lastClick_ = now;
            lastClickBtn_ = idx;
            break;
        }
        case ButtonRelease: {
            const unsigned b = ev.xbutton.button;
            const int idx = (b == Button1) ? 0 : (b == Button2) ? 1 : (b == Button3) ? 2 : -1;
            if (idx < 0) break;
            in_->down[idx] = false;
            in_->released[idx] = true;
            in_->mx = (f32)ev.xbutton.x; in_->my = (f32)ev.xbutton.y;
            break;
        }
        case KeyPress: {
            char buf[32] = {0};
            KeySym ks = 0;
            const int n = XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, nullptr);
            const int k = mapKey(ks);
            // X keycodes are evdev scancode + 8 under evdev/libinput servers.
            const unsigned sc = ev.xkey.keycode - 8;
            if (sc < 256) in_->scanDown[sc] = true;
            if (k > 0 && k < KeyCount) { in_->keyDown[k] = true; in_->keyPressed[k] = true; }
            if (n > 0 && (u8)buf[0] >= 32 && (u8)buf[0] != 127) in_->textInput.append(buf, (size_t)n);
            break;
        }
        case KeyRelease: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            const int k = mapKey(ks);
            const unsigned sc = ev.xkey.keycode - 8;
            if (sc < 256) in_->scanDown[sc] = false;
            if (k > 0 && k < KeyCount) in_->keyDown[k] = false;
            break;
        }
        case LeaveNotify: haveLast_ = false; break;
        case FocusOut:
            std::memset(in_->keyDown, 0, sizeof in_->keyDown);
            std::memset(in_->scanDown, 0, sizeof in_->scanDown);
            std::memset(in_->down, 0, sizeof in_->down);
            in_->mods = 0;
            break;
        default: break;
        }

        if (ev.type == KeyPress || ev.type == KeyRelease ||
            ev.type == ButtonPress || ev.type == ButtonRelease || ev.type == MotionNotify) {
            const unsigned st = (ev.type == MotionNotify) ? ev.xmotion.state
                              : (ev.type == KeyPress || ev.type == KeyRelease) ? ev.xkey.state
                              : ev.xbutton.state;
            in_->mods = 0;
            if (st & ShiftMask)   in_->mods |= ModShift;
            if (st & ControlMask) in_->mods |= ModCtrl;
            if (st & Mod1Mask)    in_->mods |= ModAlt;
            if (st & Mod4Mask)    in_->mods |= ModSuper;
            if (ev.type == KeyPress) {
                if (in_->keyDown[KeyShift]) in_->mods |= ModShift;
                if (in_->keyDown[KeyCtrl])  in_->mods |= ModCtrl;
                if (in_->keyDown[KeyAlt])   in_->mods |= ModAlt;
            }
        }
    }
    return !closed_;
}

} // namespace

IWindowBackend* createX11Backend() { return new X11Backend(); }

} // namespace lat
