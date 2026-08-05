// Internal interface implemented once per windowing system. Window picks one
// at runtime: Wayland when a compositor is present, X11 otherwise.
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

IWindowBackend* createX11Backend();
#if LAT_HAVE_WAYLAND
IWindowBackend* createWaylandBackend();
#endif

} // namespace lat
