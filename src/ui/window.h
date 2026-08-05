// Platform window + input. The interface is deliberately free of X11 types so
// a Win32 implementation can drop in beside window_x11.cpp.
#pragma once
#include "../core/common.h"
#include <string>

namespace lat {

enum Key : int {
    KeyNone = 0,
    // Printable keys use their ASCII code (32..126).
    KeyEnter = 0x100, KeyEscape, KeyTab, KeyBackspace, KeyDelete,
    KeyLeft, KeyRight, KeyUp, KeyDown, KeyHome, KeyEnd, KeyPageUp, KeyPageDown,
    KeyShift, KeyCtrl, KeyAlt, KeySuper,
    KeyF1, KeyF2, KeyF3, KeyF4, KeyF5, KeyF6, KeyF7, KeyF8, KeyF9, KeyF10, KeyF11, KeyF12,
    KeyCount
};

enum Mod : u32 { ModShift = 1, ModCtrl = 2, ModAlt = 4, ModSuper = 8 };

enum class Cursor { Arrow, Hand, ResizeH, ResizeV, Text, Grab };

struct Input {
    f32  mx = 0, my = 0;             // cursor, window coords
    f32  dx = 0, dy = 0;             // delta since last frame
    bool down[3]{};                  // left, middle, right held
    bool pressed[3]{};               // went down this frame
    bool released[3]{};              // went up this frame
    bool dblClick = false;
    f32  wheel = 0;                  // notches this frame, + is up
    u32  mods = 0;
    bool keyDown[KeyCount]{};
    bool keyPressed[KeyCount]{};     // includes auto-repeat
    // Physical key state, indexed by Linux evdev scancode (KEY_Z = 44, ...).
    // Layout-independent: on QWERTZ or AZERTY the bottom row is still the
    // bottom row. This is what the computer-MIDI piano maps — a piano layout
    // follows key POSITIONS, not the letters a locale prints on them.
    // Shortcuts keep using keyDown[] (Ctrl+S should follow the layout).
    // Win32 note: Set-1 make codes match evdev for the whole main block
    // (Q=0x10, Z=0x2C, ...), so the WM_KEYDOWN lParam scancode fills this
    // directly. Cleared alongside keyDown on focus loss.
    bool scanDown[256]{};
    std::string textInput;           // UTF-8 typed this frame

    bool ctrl()  const { return mods & ModCtrl; }
    bool shift() const { return mods & ModShift; }
    bool alt()   const { return mods & ModAlt; }
    void newFrame() {
        for (int i = 0; i < 3; ++i) { pressed[i] = released[i] = false; }
        for (int i = 0; i < KeyCount; ++i) keyPressed[i] = false;
        wheel = 0; dx = dy = 0; dblClick = false;
        textInput.clear();
    }
};

class Window {
public:
    bool create(const char* title, int w, int h);
    void destroy();
    bool pump();                     // false once the user closes the window
    void swap();
    void setCursor(Cursor c);
    void setTitle(const char* t);

    int   width()  const { return w_; }     // device pixels
    int   height() const { return h_; }     // device pixels
    f32   dpiScale() const { return dpi_; }
    const char* backendName() const;
    Input& input() { return in_; }

private:
    void* impl_ = nullptr;                  // IWindowBackend*
    Input in_;
    int w_ = 0, h_ = 0;
    f32 dpi_ = 1.f;
};

} // namespace lat
