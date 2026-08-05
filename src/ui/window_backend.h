// Internal interface implemented once per windowing system. Window picks one
// at runtime: Wayland when a compositor is present, X11 otherwise. On Windows
// there is exactly one, so there is nothing to pick.
#pragma once
#include "window.h"

namespace lat {

struct IWindowBackend {
    virtual ~IWindowBackend() = default;
    virtual bool create(const char* title, int w, int h, Input* in) = 0;
    virtual void destroy() = 0;
    virtual bool pump() = 0;
    virtual void swap() = 0;
    virtual void setCursor(Cursor c) = 0;
    virtual void setTitle(const char* t) = 0;
    virtual int  width()  const = 0;
    virtual int  height() const = 0;
    virtual f32  dpiScale() const = 0;
    virtual const char* name() const = 0;
};

// The factories are declared per platform rather than unconditionally: on
// Windows none of the X11/Wayland translation units exist, and an unguarded
// declaration is exactly how window.cpp ended up with an unresolvable
// createX11Backend() call in the Windows build.
#if defined(_WIN32)
IWindowBackend* createWin32Backend();       // window_win32.cpp
#else
IWindowBackend* createX11Backend();         // window_x11.cpp
#if LAT_HAVE_WAYLAND
IWindowBackend* createWaylandBackend();     // window_wayland.cpp
#endif
#endif

} // namespace lat
