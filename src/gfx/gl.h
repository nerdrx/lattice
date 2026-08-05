// OpenGL entry points.
//
// We deliberately do NOT use GLEW: it is built against GLX on most distros and
// refuses to initialise under a pure EGL context, which is exactly what the
// native Wayland backend creates. On Linux libGL exports the full core profile
// directly, so declaring the prototypes is enough.
//
// Windows is the exception -- opengl32.dll only exports GL 1.1 -- so the Win32
// backend has to resolve the 2.0+ entry points through wglGetProcAddress. That
// loader lives with that backend; everything above this header is unaffected.
#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#include "gl_win32_loader.h"
#else
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#endif
