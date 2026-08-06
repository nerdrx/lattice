#!/bin/sh
# scan_deps.sh -- list the third-party libraries a set of sources pulls in.
#
#   tools/scan_deps.sh tests/engine_test.cpp src/audio/engine.cpp ...
#
# Walks the transitive closure of  #include "..."  from the given roots and
# prints, one per line, every  #include <...>  in that closure that is not a
# C or C++ standard header and not an OS header. In other words: the external
# libraries this build actually needs, derived from the code rather than from
# whatever the link line happens to say.
#
# Why this exists
# ---------------
# Makefile.mingw's verify-sources compares the *prerequisite lists* of the two
# engine_test rules, so the native and cross builds cannot drift apart in which
# files they compile. That check has a blind spot, and the blind spot bit:
# tests/engine_test.cpp gained
#
#     #include "../src/audio/sample.cpp"
#
# to reach detectTransients(). The prerequisite lists still matched perfectly --
# no rule changed -- but sample.cpp includes <sndfile.h> and <samplerate.h>,
# neither of which exists cross-built for mingw. The Windows job had been red
# for six commits before anyone read the log, because the guard was watching
# the link line while the dependency arrived through an #include.
#
# A closure scan sees it. Sources are the ground truth; makefiles are a claim
# about sources.
#
# Deliberately textual: no compiler, no preprocessor, no toolchain required, so
# it runs identically on a runner with no mingw installed and on a developer
# box. It therefore does not evaluate #if -- a header behind a conditional is
# still reported. For "which libraries could this need", over-reporting is the
# safe direction.

set -eu

[ $# -gt 0 ] || { echo "usage: $0 <source>..." >&2; exit 2; }

# Resolve to the repo root so relative includes resolve the same way the
# compiler resolves them: relative to the including file's own directory.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

seen_list=""      # files already walked, each wrapped in | for substring matching
depth=0           # recursion level, used to give each frame its own scratch file

# Standard C++ headers, C headers in both spellings, and OS headers. Anything
# not matched here is treated as third-party -- the safe default, since a new
# library is exactly what this is meant to catch.
is_system() {
    case $1 in
        # C++ standard library: no dot, no slash.
        *.*|*/*) ;;
        *) return 0 ;;
    esac
    case $1 in
        # C standard library, both spellings.
        assert.h|complex.h|ctype.h|errno.h|fenv.h|float.h|inttypes.h|iso646.h|\
        limits.h|locale.h|math.h|setjmp.h|signal.h|stdalign.h|stdarg.h|\
        stdatomic.h|stdbool.h|stddef.h|stdint.h|stdio.h|stdlib.h|stdnoreturn.h|\
        string.h|tgmath.h|threads.h|time.h|uchar.h|wchar.h|wctype.h) return 0 ;;
        # POSIX and Linux.
        unistd.h|fcntl.h|dlfcn.h|pthread.h|semaphore.h|sched.h|poll.h|dirent.h|\
        termios.h|syslog.h|glob.h|libgen.h|strings.h|sys/*|linux/*|netinet/*|\
        arpa/*|net/*) return 0 ;;
        # Windows.
        windows.h|windowsx.h|winsock2.h|ws2tcpip.h|shellapi.h|shlobj.h|\
        objbase.h|combaseapi.h|mmdeviceapi.h|audioclient.h|audiopolicy.h|\
        functiondiscoverykeys_devpkey.h|avrt.h|shellscalingapi.h|versionhelpers.h|\
        initguid.h|propidl.h|propvarutil.h|knownfolders.h|timeapi.h|\
        mmsystem.h|processthreadsapi.h|synchapi.h|memoryapi.h|handleapi.h|\
        fileapi.h|errhandlingapi.h|libloaderapi.h|winbase.h|winuser.h) return 0 ;;
        # GL comes from the OS/driver on every target we build, not from a
        # package we would have to cross-build.
        GL/*|EGL/*|KHR/*|GLES2/*|GLES3/*) return 0 ;;
        # Compiler-provided intrinsics. Shipped by gcc and clang themselves --
        # including the mingw cross-compilers -- so they are never a packaging
        # problem. engine.cpp uses xmmintrin/pmmintrin for the FTZ+DAZ bits.
        *intrin.h|cpuid.h) return 0 ;;
    esac
    return 1
}

# POSIX sh has no locals: every variable here is global, so a nested walk()
# clobbers its caller's. Two consequences, both of which bit the first draft:
#
#   * the scratch file must be named per frame. A single shared name means a
#     nested call truncates the file its caller is still reading, and the outer
#     loop ends after one entry -- which is how this script first reported zero
#     dependencies for a tree that plainly has them.
#   * everything derived from $1 must be consumed BEFORE recursing, because
#     $file and $dir will not survive the first nested call.
#
# So each frame does all of its own reading up front, into $tmp/local.$depth and
# $tmp/sys.$depth, and only then walks its children. $depth is restored by each
# nested call, so recomputing the names after the loop addresses this frame's
# files again.
walk() {
    [ -f "$1" ] || return 0
    # Normalise so the same file reached by two paths is walked once.
    key=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
    case $seen_list in *"|$key|"*) return 0 ;; esac
    seen_list="$seen_list|$key|"

    depth=$((depth + 1))
    file=$1
    dir=$(dirname -- "$file")

    # Comments stripped first, so an #include inside /* */ or after // is
    # neither followed nor reported -- cheap sed, whole class of false positive.
    #
    # Local includes: resolved against this file's directory, for recursion.
    sed 's://.*::; s:/\*.*\*/::' "$file" 2>/dev/null \
      | sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' \
      | sed "s:^:$dir/:" > "$tmp/local.$depth"

    # System includes: recorded, never recursed into -- we cannot see inside a
    # system header and do not need to. The library name is the thing reported.
    sed 's://.*::; s:/\*.*\*/::' "$file" 2>/dev/null \
      | sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*<\([^>]*\)>.*/\1/p' \
      | while IFS= read -r inc; do
            is_system "$inc" || printf '%s\n' "$inc"
        done >> "$tmp/third"

    # $file and $dir are dead from here on; only $depth is trusted.
    while IFS= read -r target; do
        [ -n "$target" ] || continue
        walk "$target"
    done < "$tmp/local.$depth"

    rm -f "$tmp/local.$depth"
    depth=$((depth - 1))
}

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT INT TERM HUP
: > "$tmp/third"

for f in "$@"; do
    case $f in
        /*) walk "$f" ;;
         *) walk "$root/$f" ;;
    esac
done

sort -u "$tmp/third"
