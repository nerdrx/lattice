#!/usr/bin/env bash
# Runs Lattice inside a headless gamescope compositor and captures a screenshot,
# so UI checks never open a window on the developer's desktop.
#
#   tools/headless_test.sh [-o out.png] [-W 1600] [-H 1000] [-s secs] [--wayland] [-- args...]
#
# --wayland exposes gamescope's own Wayland socket to the child so the native
# backend is exercised; without it the child gets XWayland and takes the X11
# path. Both are worth testing.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=/tmp/lattice_headless.png
W=1600; H=1000; SETTLE=4; EXPOSE=0
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUT="$2"; shift 2 ;;
        -W) W="$2"; shift 2 ;;
        -H) H="$2"; shift 2 ;;
        -s) SETTLE="$2"; shift 2 ;;
        --wayland) EXPOSE=1; shift ;;
        --) shift; ARGS=("$@"); break ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

command -v gamescope    >/dev/null || { echo "gamescope not installed"; exit 1; }
command -v gamescopectl >/dev/null || { echo "gamescopectl not installed"; exit 1; }
[[ -x build/lattice ]]              || { echo "build/lattice missing - run make"; exit 1; }

LOG=$(mktemp /tmp/lattice-headless-XXXXXX.log)
GS_ARGS=(--backend headless -W "$W" -H "$H" -w "$W" -h "$H")
[[ $EXPOSE -eq 1 ]] && GS_ARGS+=(--expose-wayland)

# Claim a private socket name so a real gamescope session on the desktop is
# never touched by our screenshot or our kill.
export GAMESCOPE_WAYLAND_DISPLAY=""
gamescope "${GS_ARGS[@]}" -- ./build/lattice "${ARGS[@]}" >"$LOG" 2>&1 &
GS_PID=$!
cleanup() { kill "$GS_PID" 2>/dev/null; wait "$GS_PID" 2>/dev/null; }
trap cleanup EXIT

# Wait for the nested compositor to publish its socket.
SOCK=""
for _ in $(seq 1 100); do
    for s in "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/gamescope-*; do
        [[ -S "$s" ]] || continue
        cand=$(basename "$s")
        [[ "$cand" == *.lock || "$cand" == *-ei ]] && continue
        SOCK="$cand"
    done
    [[ -n "$SOCK" ]] && break
    sleep 0.1
done
[[ -n "$SOCK" ]] || { echo "gamescope never came up:"; tail -20 "$LOG"; exit 1; }

sleep "$SETTLE"     # let the app load its project and render a few frames

if ! GAMESCOPE_WAYLAND_DISPLAY="$SOCK" gamescopectl screenshot "$OUT" >/dev/null 2>&1; then
    echo "screenshot failed"; tail -20 "$LOG"; exit 1
fi
for _ in $(seq 1 50); do [[ -s "$OUT" ]] && break; sleep 0.1; done

echo "=== lattice output ==="
grep -aE "^\[lattice" "$LOG" | head -30
echo "=== screenshot: $OUT ($(stat -c%s "$OUT" 2>/dev/null || echo 0) bytes) ==="
echo "=== full log: $LOG ==="
