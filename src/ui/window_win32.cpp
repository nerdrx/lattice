// Win32 + WGL backend. Mirrors window_x11.cpp one-for-one; the whole file
// compiles to nothing on non-Windows hosts so it can sit in the same source
// tree as the Linux backends without any build-system filtering.
#if defined(_WIN32)

#include "window_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <cstring>
#include <string>

// No <GL/gl.h> or <GL/glew.h> here on purpose: everything this backend needs
// (HGLRC, PIXELFORMATDESCRIPTOR, wgl*, SwapBuffers) lives in wingdi.h, and
// staying out of glew.h avoids its include-order rules in a TU that must
// include windows.h first anyway. renderer.cpp owns glewInit().

// ---------------------------------------------------------------------------
// WGL extension bits. Declared locally with non-standard type names so this
// still compiles if some other header ever drags in wglext.h.
// ---------------------------------------------------------------------------
#ifndef WGL_DRAW_TO_WINDOW_ARB
#define WGL_DRAW_TO_WINDOW_ARB            0x2001
#define WGL_ACCELERATION_ARB              0x2003
#define WGL_SUPPORT_OPENGL_ARB            0x2010
#define WGL_DOUBLE_BUFFER_ARB             0x2011
#define WGL_PIXEL_TYPE_ARB                0x2013
#define WGL_COLOR_BITS_ARB                0x2014
#define WGL_RED_BITS_ARB                  0x2015
#define WGL_GREEN_BITS_ARB                0x2017
#define WGL_BLUE_BITS_ARB                 0x2019
#define WGL_ALPHA_BITS_ARB                0x201B
#define WGL_DEPTH_BITS_ARB                0x2022
#define WGL_STENCIL_BITS_ARB              0x2023
#define WGL_FULL_ACCELERATION_ARB         0x2027
#define WGL_TYPE_RGBA_ARB                 0x202B
#endif
#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_FLAGS_ARB             0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef MAPVK_VK_TO_CHAR
#define MAPVK_VK_TO_CHAR 2
#endif

namespace lat {
namespace {

using WglChoosePixelFormatFn    = BOOL  (WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using WglCreateContextAttribsFn = HGLRC (WINAPI*)(HDC, HGLRC, const int*);
using WglSwapIntervalFn         = BOOL  (WINAPI*)(int);

struct WglFns {
    WglChoosePixelFormatFn    choosePixelFormat = nullptr;
    WglCreateContextAttribsFn createContext     = nullptr;
    WglSwapIntervalFn         swapInterval      = nullptr;
};

constexpr const wchar_t* kClassName = L"NxTaktWindow";
constexpr const wchar_t* kBootClassName = L"NxTaktWglBootstrap";

// GCC warns about casting FARPROC to a concrete signature; the void* hop is the
// blessed way to shut it up without disabling the warning globally.
template <typename Fn> Fn wglProc(const char* n) { return (Fn)(void*)wglGetProcAddress(n); }
template <typename Fn> Fn dllProc(HMODULE m, const char* n) { return m ? (Fn)(void*)GetProcAddress(m, n) : nullptr; }

HMODULE user32() {
    static HMODULE m = GetModuleHandleW(L"user32.dll");
    return m;
}

// Must run before the first window (including the WGL bootstrap one) exists,
// otherwise Windows locks the process into system-DPI awareness for good.
void enablePerMonitorDpi() {
    // Loaded dynamically: the V2 context is Windows 10 1703+, and hard-linking
    // it would make the binary refuse to start on anything older.
    using SetCtxFn   = BOOL (WINAPI*)(HANDLE);
    using SetAwareFn = BOOL (WINAPI*)(void);
    if (auto f = dllProc<SetCtxFn>(user32(), "SetProcessDpiAwarenessContext")) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4. Spelled out
        // as a literal so we do not depend on the SDK version's windef.h.
        // Via INT_PTR: a bare (HANDLE)-4 is an int-to-64-bit-pointer cast, which
        // is implementation-defined and warns under -Wint-to-pointer-cast on
        // some targets. Widening to pointer size first makes the sign extension
        // explicit and the value exactly 0xFFFF'FFFF'FFFF'FFFC.
        if (f((HANDLE)(INT_PTR)-4)) return;
    }
    if (auto f = dllProc<SetAwareFn>(user32(), "SetProcessDPIAware")) f();
}

UINT dpiForWindow(HWND h) {
    using Fn = UINT (WINAPI*)(HWND);
    if (auto f = dllProc<Fn>(user32(), "GetDpiForWindow")) {
        if (const UINT d = f(h)) return d;
    }
    HDC dc = GetDC(nullptr);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

BOOL adjustRect(RECT* r, DWORD style, DWORD exStyle, UINT dpi) {
    using Fn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (auto f = dllProc<Fn>(user32(), "AdjustWindowRectExForDpi"))
        return f(r, style, FALSE, exStyle, dpi);
    return AdjustWindowRectEx(r, style, FALSE, exStyle);
}

int mapKey(UINT vk) {
    switch (vk) {
    case VK_RETURN:  return KeyEnter;
    case VK_ESCAPE:  return KeyEscape;
    case VK_TAB:     return KeyTab;
    case VK_BACK:    return KeyBackspace;
    case VK_DELETE:  return KeyDelete;
    case VK_LEFT:    return KeyLeft;
    case VK_RIGHT:   return KeyRight;
    case VK_UP:      return KeyUp;
    case VK_DOWN:    return KeyDown;
    case VK_HOME:    return KeyHome;
    case VK_END:     return KeyEnd;
    case VK_PRIOR:   return KeyPageUp;
    case VK_NEXT:    return KeyPageDown;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:       return KeyShift;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return KeyCtrl;
    case VK_MENU: case VK_LMENU: case VK_RMENU:          return KeyAlt;
    case VK_LWIN: case VK_RWIN:                          return KeySuper;
    case VK_F1: return KeyF1;   case VK_F2: return KeyF2;   case VK_F3: return KeyF3;
    case VK_F4: return KeyF4;   case VK_F5: return KeyF5;   case VK_F6: return KeyF6;
    case VK_F7: return KeyF7;   case VK_F8: return KeyF8;   case VK_F9: return KeyF9;
    case VK_F10: return KeyF10; case VK_F11: return KeyF11; case VK_F12: return KeyF12;
    case VK_SPACE: return ' ';
    default: break;
    }
    if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= 'A' && vk <= 'Z') return (int)(vk - 'A' + 'a');
    // Numpad digits fold onto the top row so numeric shortcuts fire either way.
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (int)('0' + (vk - VK_NUMPAD0));

    // Punctuation VKs are layout dependent. Ask the active layout for the
    // unshifted character instead of baking in a US mapping, which is the
    // closest analogue to X11 handing us a keysym.
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (ch >= 32 && ch <= 126)
        return (ch >= 'A' && ch <= 'Z') ? (int)(ch - 'A' + 'a') : (int)ch;
    return KeyNone;
}

// Physical key position for Input::scanDown, which is indexed by Linux evdev
// scancode. The lParam of WM_KEYDOWN/WM_KEYUP carries the PS/2 Set-1 make code
// in bits 16..23, and for the whole main block Set-1 and evdev agree exactly
// (1..0 = 0x02..0x0B, Q = 0x10, A = 0x1E, Z = 0x2C, space = 0x39) because
// evdev's keycode table was derived from Set 1 in the first place. That is the
// only region the computer-MIDI piano cares about, so no translation table.
//
// Bit 24 is the extended flag: those keys (arrows, Home/End, right Ctrl/Alt,
// numpad Enter, the Windows keys) reuse main-block make codes with an E0
// prefix, so 0x1C would arrive both as Enter and as numpad Enter. They are
// never piano keys, so the cheapest correct answer is to drop them entirely
// rather than fold them onto a key someone might be holding.
//
// Returns -1 when there is nothing usable to record.
int scanCode(LPARAM lp) {
    if ((lp >> 24) & 1) return -1;              // extended (E0/E1 prefixed)
    const int sc = (int)((lp >> 16) & 0xFF);
    return (sc > 0 && sc < 256) ? sc : -1;      // 0 = injected/synthetic event
}

void appendUtf8(std::string& out, const wchar_t* w, int n) {
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    const size_t at = out.size();
    out.resize(at + (size_t)need);
    WideCharToMultiByte(CP_UTF8, 0, w, n, &out[at], need, nullptr, nullptr);
}

std::wstring widen(const char* s) {
    if (!s || !*s) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w((size_t)need - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], need);
    return w;
}

// The two-step context dance: a modern pixel format can only be chosen through
// wglChoosePixelFormatARB, which can only be resolved from a context, which
// needs a pixel format. So we burn a throwaway window on a legacy format,
// harvest the entry points, and tear it back down before touching the real one.
bool loadWglFns(WglFns& out) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = inst;
    wc.lpszClassName = kBootClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOGE("cannot register the WGL bootstrap window class");
        return false;
    }

    HWND hwnd = CreateWindowExW(0, kBootClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd) { UnregisterClassW(kBootClassName, inst); return false; }

    HDC   hdc  = GetDC(hwnd);
    HGLRC ctx  = nullptr;
    bool  ok   = false;

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize      = sizeof pfd;
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;

    const int fmt = hdc ? ChoosePixelFormat(hdc, &pfd) : 0;
    if (fmt && SetPixelFormat(hdc, fmt, &pfd)) {
        ctx = wglCreateContext(hdc);
        if (ctx && wglMakeCurrent(hdc, ctx)) {
            out.choosePixelFormat = wglProc<WglChoosePixelFormatFn>("wglChoosePixelFormatARB");
            out.createContext     = wglProc<WglCreateContextAttribsFn>("wglCreateContextAttribsARB");
            out.swapInterval      = wglProc<WglSwapIntervalFn>("wglSwapIntervalEXT");
            ok = out.choosePixelFormat != nullptr && out.createContext != nullptr;
        }
    }

    wglMakeCurrent(nullptr, nullptr);
    if (ctx) wglDeleteContext(ctx);
    if (hdc) ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    UnregisterClassW(kBootClassName, inst);

    if (!ok) LOGE("driver has no WGL_ARB_pixel_format / WGL_ARB_create_context");
    return ok;
}

class Win32Backend final : public IWindowBackend {
public:
    bool create(const char* title, int w, int h, Input* in) override;
    void destroy() override;
    bool pump() override;
    void swap() override { if (hdc_) SwapBuffers(hdc_); }
    void setCursor(Cursor c) override {
        if (!hwnd_ || c == curCursor_) return;
        curCursor_ = c;
        // WM_SETCURSOR reasserts this every time the pointer moves; setting it
        // here too makes the change visible without waiting for a move.
        SetCursor(cursors_[(int)c]);
    }
    void setTitle(const char* t) override { if (hwnd_) SetWindowTextW(hwnd_, widen(t).c_str()); }
    int  width()  const override { return w_; }
    int  height() const override { return h_; }
    f32  dpiScale() const override { return dpi_; }
    const char* name() const override { return "Win32/WGL"; }

private:
    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND h, UINT m, WPARAM wp, LPARAM lp);

    void syncMods();
    void onButton(HWND h, int idx, bool down, LPARAM lp, bool dbl);

    HWND  hwnd_ = nullptr;
    HDC   hdc_  = nullptr;
    HGLRC ctx_  = nullptr;
    WglFns wgl_{};
    HCURSOR cursors_[6]{};
    Cursor  curCursor_ = Cursor::Arrow;
    Input*  in_ = nullptr;
    bool closed_ = false;
    bool tracking_ = false;          // TrackMouseEvent armed for WM_MOUSELEAVE
    int  buttons_ = 0;               // held-button count, drives SetCapture
    wchar_t pendingHigh_ = 0;        // half of a UTF-16 surrogate pair
    f32  lastX_ = 0, lastY_ = 0;
    bool haveLast_ = false;
    int  w_ = 0, h_ = 0;
    f32  dpi_ = 1.f;
};

bool Win32Backend::create(const char* title, int w, int h, Input* in) {
    in_ = in;
    enablePerMonitorDpi();
    if (!loadWglFns(wgl_)) return false;

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    // CS_OWNDC keeps one DC alive for the GL context; CS_DBLCLKS is what turns
    // the second click into WM_LBUTTONDBLCLK at all.
    wc.style         = CS_OWNDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Win32Backend::wndProc;
    wc.hInstance     = inst;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = nullptr;       // we answer WM_SETCURSOR ourselves
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOGE("RegisterClassExW failed (%lu)", (unsigned long)GetLastError());
        return false;
    }

    const DWORD style = WS_OVERLAPPEDWINDOW;
    hwnd_ = CreateWindowExW(0, kClassName, widen(title).c_str(), style,
                            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) { LOGE("CreateWindowExW failed (%lu)", (unsigned long)GetLastError()); destroy(); return false; }

    // Window::create() deletes the backend without calling destroy() when we
    // return false, so every failure past this point has to unwind itself.
    hdc_ = GetDC(hwnd_);
    if (!hdc_) { LOGE("GetDC failed"); destroy(); return false; }

    // 1 rather than GL_TRUE: no GL header is included here (see the note above).
    const int pfAttribs[] = {
        WGL_DRAW_TO_WINDOW_ARB, 1,
        WGL_SUPPORT_OPENGL_ARB, 1,
        WGL_DOUBLE_BUFFER_ARB,  1,
        WGL_ACCELERATION_ARB,   WGL_FULL_ACCELERATION_ARB,
        WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_ARB,
        WGL_RED_BITS_ARB,   8,
        WGL_GREEN_BITS_ARB, 8,
        WGL_BLUE_BITS_ARB,  8,
        WGL_ALPHA_BITS_ARB, 8,
        WGL_COLOR_BITS_ARB, 32,
        WGL_DEPTH_BITS_ARB,   0,
        WGL_STENCIL_BITS_ARB, 0,
        0
    };
    int  fmt = 0;
    UINT nfmt = 0;
    if (!wgl_.choosePixelFormat(hdc_, pfAttribs, nullptr, 1, &fmt, &nfmt) || nfmt == 0) {
        LOGE("no suitable WGL pixel format");
        destroy();
        return false;
    }
    PIXELFORMATDESCRIPTOR pfd{};
    DescribePixelFormat(hdc_, fmt, sizeof pfd, &pfd);
    if (!SetPixelFormat(hdc_, fmt, &pfd)) { LOGE("SetPixelFormat failed"); destroy(); return false; }

    const int ctxAttribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    ctx_ = wgl_.createContext(hdc_, nullptr, ctxAttribs);
    // No legacy fallback on purpose: wglCreateContext would hand back a 1.1
    // context, and the renderer needs 3.3 core. Better to fail loudly.
    if (!ctx_) { LOGE("cannot create an OpenGL 3.3 core context"); destroy(); return false; }
    if (!wglMakeCurrent(hdc_, ctx_)) { LOGE("wglMakeCurrent failed"); destroy(); return false; }

    if (wgl_.swapInterval) wgl_.swapInterval(1);

    cursors_[(int)Cursor::Arrow]   = LoadCursorW(nullptr, IDC_ARROW);
    cursors_[(int)Cursor::Hand]    = LoadCursorW(nullptr, IDC_HAND);
    cursors_[(int)Cursor::ResizeH] = LoadCursorW(nullptr, IDC_SIZEWE);
    cursors_[(int)Cursor::ResizeV] = LoadCursorW(nullptr, IDC_SIZENS);
    cursors_[(int)Cursor::Text]    = LoadCursorW(nullptr, IDC_IBEAM);
    cursors_[(int)Cursor::Grab]    = LoadCursorW(nullptr, IDC_SIZEALL);

    // The caller's size is logical, like on X11. We only learn the real DPI
    // once the window exists on a monitor, so size it properly after creation.
    const UINT rawDpi = dpiForWindow(hwnd_);
    dpi_ = (f32)rawDpi / 96.f;
    RECT want{0, 0, (LONG)(w * dpi_), (LONG)(h * dpi_)};
    adjustRect(&want, style, 0, rawDpi);
    SetWindowPos(hwnd_, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    RECT cr{};
    GetClientRect(hwnd_, &cr);
    w_ = cr.right - cr.left;
    h_ = cr.bottom - cr.top;

    LOGI("Win32 window %dx%d, scale %.2f", w_, h_, dpi_);
    return true;
}

void Win32Backend::destroy() {
    if (ctx_) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(ctx_); ctx_ = nullptr; }
    if (hwnd_ && hdc_) { ReleaseDC(hwnd_, hdc_); hdc_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

bool Win32Backend::pump() {
    if (!hwnd_) return false;
    in_->newFrame();

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { closed_ = true; continue; }
        TranslateMessage(&msg);      // synthesises WM_CHAR from WM_KEYDOWN
        DispatchMessageW(&msg);
    }
    return !closed_;
}

LRESULT CALLBACK Win32Backend::wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Win32Backend* self;
    if (m == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (Win32Backend*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->hwnd_ = h;
    } else {
        self = (Win32Backend*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    // Messages arrive before WM_NCCREATE and after we drop the pointer; those
    // go straight to the default handler.
    return self ? self->handle(h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

void Win32Backend::syncMods() {
    if (!in_) return;
    u32 mods = 0;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= ModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= ModCtrl;
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= ModAlt;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= ModSuper;
    in_->mods = mods;
}

void Win32Backend::onButton(HWND h, int idx, bool down, LPARAM lp, bool dbl) {
    in_->mx = (f32)GET_X_LPARAM(lp);
    in_->my = (f32)GET_Y_LPARAM(lp);
    if (down) {
        in_->down[idx] = true;
        in_->pressed[idx] = true;
        if (dbl) in_->dblClick = true;
        // Capture so a drag that leaves the client area keeps delivering moves,
        // matching X11's implicit pointer grab on button press.
        if (buttons_++ == 0) SetCapture(h);
    } else {
        in_->down[idx] = false;
        in_->released[idx] = true;
        if (buttons_ > 0 && --buttons_ == 0) ReleaseCapture();
    }
    syncMods();
}

LRESULT Win32Backend::handle(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CLOSE:
        closed_ = true;
        return 0;                    // destroy() owns the teardown, not the WM

    case WM_DESTROY:
        closed_ = true;
        return 0;

    case WM_ERASEBKGND:
        return 1;                    // GL owns every pixel; GDI erasing only flickers

    case WM_PAINT:
        // The render loop redraws unconditionally, so the only job here is to
        // validate the region — leaving it dirty makes Windows resend WM_PAINT
        // forever and pump() would spin.
        ValidateRect(h, nullptr);
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { w_ = LOWORD(lp); h_ = HIWORD(lp); }
        return 0;

    case WM_GETMINMAXINFO: {
        if (dpi_ <= 0.f) break;
        RECT r{0, 0, (LONG)(900 * dpi_), (LONG)(560 * dpi_)};
        adjustRect(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), 0, (UINT)(dpi_ * 96.f));
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = (f32)HIWORD(wp) / 96.f;
        // Windows hands us the rect that keeps the window the same physical
        // size on the new monitor; ignoring it makes dragging across monitors
        // resize the window unpredictably.
        const RECT* sug = (const RECT*)lp;
        SetWindowPos(h, nullptr, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(cursors_[(int)curCursor_]); return TRUE; }
        break;                       // frame/borders keep the system cursors

    case WM_MOUSEMOVE: {
        const f32 x = (f32)GET_X_LPARAM(lp), y = (f32)GET_Y_LPARAM(lp);
        if (haveLast_) { in_->dx += x - lastX_; in_->dy += y - lastY_; }
        lastX_ = x; lastY_ = y; haveLast_ = true;
        in_->mx = x; in_->my = y;
        syncMods();
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        // Drop the delta anchor, otherwise re-entering the window teleports it.
        tracking_ = false;
        haveLast_ = false;
        return 0;

    case WM_LBUTTONDOWN:   onButton(h, 0, true,  lp, false); return 0;
    case WM_LBUTTONUP:     onButton(h, 0, false, lp, false); return 0;
    case WM_MBUTTONDOWN:   onButton(h, 1, true,  lp, false); return 0;
    case WM_MBUTTONUP:     onButton(h, 1, false, lp, false); return 0;
    case WM_RBUTTONDOWN:   onButton(h, 2, true,  lp, false); return 0;
    case WM_RBUTTONUP:     onButton(h, 2, false, lp, false); return 0;
    // With CS_DBLCLKS the second press arrives as DBLCLK *instead of* DOWN, so
    // these must also register the press or the click would be swallowed.
    case WM_LBUTTONDBLCLK: onButton(h, 0, true,  lp, true);  return 0;
    case WM_MBUTTONDBLCLK: onButton(h, 1, true,  lp, true);  return 0;
    case WM_RBUTTONDBLCLK: onButton(h, 2, true,  lp, true);  return 0;

    case WM_MOUSEWHEEL:
        // lParam here is in screen coords, so mx/my deliberately stay put.
        in_->wheel += (f32)GET_WHEEL_DELTA_WPARAM(wp) / (f32)WHEEL_DELTA;
        syncMods();
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const int k = mapKey((UINT)wp);
        if (k > 0 && k < KeyCount) {
            in_->keyDown[k] = true;
            in_->keyPressed[k] = true;   // auto-repeat included, as on X11
        }
        if (const int sc = scanCode(lp); sc >= 0) in_->scanDown[sc] = true;
        syncMods();
        // Sys keys fall through so Alt+F4 and Alt+Space keep working.
        if (m == WM_SYSKEYDOWN) break;
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const int k = mapKey((UINT)wp);
        if (k > 0 && k < KeyCount) in_->keyDown[k] = false;
        if (const int sc = scanCode(lp); sc >= 0) in_->scanDown[sc] = false;
        syncMods();
        if (m == WM_SYSKEYUP) break;
        return 0;
    }

    case WM_SYSCOMMAND:
        // Swallow the Alt / F10 menu activation: there is no menu bar, and the
        // default handler beeps and eats keyboard focus.
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;

    case WM_CHAR: {
        const wchar_t c = (wchar_t)wp;
        wchar_t buf[2];
        int n = 0;
        if (c >= 0xD800 && c <= 0xDBFF) { pendingHigh_ = c; return 0; }
        if (c >= 0xDC00 && c <= 0xDFFF) {
            if (!pendingHigh_) return 0;
            buf[0] = pendingHigh_; buf[1] = c; n = 2;
            pendingHigh_ = 0;
        } else {
            pendingHigh_ = 0;
            if (c < 32 || c == 127) return 0;   // control codes are not text
            buf[0] = c; n = 1;
        }
        appendUtf8(in_->textInput, buf, n);
        return 0;
    }

    case WM_KILLFOCUS:
        // Keys released while we were not focused never generate WM_KEYUP.
        std::memset(in_->keyDown, 0, sizeof in_->keyDown);
        std::memset(in_->scanDown, 0, sizeof in_->scanDown);
        std::memset(in_->down, 0, sizeof in_->down);
        in_->mods = 0;
        haveLast_ = false;
        buttons_ = 0;
        pendingHigh_ = 0;
        return 0;

    case WM_CAPTURECHANGED:
        // Only ever delivered when we *lose* capture, so a drag was stolen
        // mid-flight; without this the buttons would stay stuck down.
        std::memset(in_->down, 0, sizeof in_->down);
        buttons_ = 0;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace

IWindowBackend* createWin32Backend() { return new Win32Backend(); }

} // namespace lat

#endif // _WIN32
