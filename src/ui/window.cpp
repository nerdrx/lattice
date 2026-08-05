#include "window_backend.h"
#include <cstdlib>
#include <cstring>

namespace lat {

bool Window::create(const char* title, int w, int h) {
#if defined(_WIN32)
    // One backend, no probing: LATTICE_BACKEND has nothing to select between,
    // so it is deliberately ignored rather than silently half-honoured.
    if (IWindowBackend* b = createWin32Backend()) {
        if (b->create(title, w, h, &in_)) {
            impl_ = b;
            w_ = b->width(); h_ = b->height(); dpi_ = b->dpiScale();
            return true;
        }
        delete b;
    }
    LOGE("no usable window backend");
    return false;
#else
    const char* force = std::getenv("LATTICE_BACKEND");
    const bool wantWayland = force ? std::strcmp(force, "wayland") == 0
                                   : std::getenv("WAYLAND_DISPLAY") != nullptr;
    const bool wantX11 = force ? std::strcmp(force, "x11") == 0 : true;

#if LAT_HAVE_WAYLAND
    if (wantWayland) {
        if (IWindowBackend* b = createWaylandBackend()) {
            if (b->create(title, w, h, &in_)) {
                impl_ = b;
                w_ = b->width(); h_ = b->height(); dpi_ = b->dpiScale();
                return true;
            }
            delete b;
            LOGW("Wayland backend failed; trying X11");
        }
    }
#else
    if (wantWayland)
        LOGW("built without Wayland support; using X11 (XWayland)");
#endif

    if (wantX11 || !force) {
        if (IWindowBackend* b = createX11Backend()) {
            if (b->create(title, w, h, &in_)) {
                impl_ = b;
                w_ = b->width(); h_ = b->height(); dpi_ = b->dpiScale();
                return true;
            }
            delete b;
        }
    }
    LOGE("no usable window backend");
    return false;
#endif // _WIN32
}

void Window::destroy() {
    if (!impl_) return;
    auto* b = (IWindowBackend*)impl_;
    b->destroy();
    delete b;
    impl_ = nullptr;
}

bool Window::pump() {
    if (!impl_) return false;
    auto* b = (IWindowBackend*)impl_;
    const bool alive = b->pump();
    w_ = b->width(); h_ = b->height(); dpi_ = b->dpiScale();
    return alive;
}

void Window::swap()                    { if (impl_) ((IWindowBackend*)impl_)->swap(); }
void Window::setCursor(Cursor c)       { if (impl_) ((IWindowBackend*)impl_)->setCursor(c); }
void Window::setTitle(const char* t)   { if (impl_) ((IWindowBackend*)impl_)->setTitle(t); }
const char* Window::backendName() const {
    return impl_ ? ((IWindowBackend*)impl_)->name() : "none";
}

} // namespace lat
