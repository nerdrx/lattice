#!/bin/bash
# Shared apt helpers for the CI workflows. Source it, do not execute it:
#
#   source .github/scripts/apt.sh
#   apt_update
#   apt_install mingw-w64
#   apt_install_first_available "wine wine64" "wine64" "wine"
#
# Why this exists
# ---------------
# The Windows job failed once on a commit whose inputs were byte-identical to
# the commit before it -- same engine_test.cpp, same engine.cpp, same
# Makefile.mingw, same workflow file. Identical inputs, opposite outcomes: the
# code could not have been the cause, which leaves the runner environment, and
# on GitHub runners that overwhelmingly means a transient apt failure (a mirror
# closing a connection mid-fetch, a hash mismatch while a mirror syncs).
#
# Every apt call in these workflows was unguarded, so any one of them could red
# the build for reasons having nothing to do with this repository. A red build
# nobody can explain is worse than no build at all: it trains everyone to
# ignore the badge.
#
# Retries are bounded and they are LOUD. Each attempt prints, and a retry
# prints why it is retrying, so a genuinely broken package list still fails --
# it just fails after saying so three times instead of once. This never
# converts a real failure into a pass; it only absorbs the flaky ones.

set -euo pipefail

# Three attempts, backing off 5s then 15s. Long enough to outlast a mirror
# hiccup, short enough that a genuine failure still reports inside a minute.
_apt_try() {
    local what="$1"; shift
    local delay
    for attempt in 1 2 3; do
        if "$@"; then
            [ "$attempt" -gt 1 ] && echo "::notice::$what succeeded on attempt $attempt"
            return 0
        fi
        if [ "$attempt" -lt 3 ]; then
            delay=$(( attempt == 1 ? 5 : 15 ))
            echo "::warning::$what failed (attempt $attempt/3), retrying in ${delay}s"
            sleep "$delay"
        fi
    done
    echo "::error::$what failed three times -- this is not a flake"
    return 1
}

apt_update() {
    _apt_try "apt-get update" sudo apt-get update -qq
}

apt_install() {
    _apt_try "apt-get install $*" \
        sudo apt-get install -y --no-install-recommends "$@"
}

# Installs the first candidate set whose packages all exist in the index.
#
# Replaces the `install A || install B || install C` chain the wine step used.
# That chain works, but combined with retries it would spend 20 seconds failing
# three times over a package that simply is not on this image before moving on.
# Asking the index which spelling exists is both faster and more honest about
# what it is doing: package names differ across Ubuntu releases (wine64 was
# merged into wine on noble), and that is a question apt-cache can answer
# directly rather than something to discover by failing.
apt_install_first_available() {
    local candidate pkg ok
    for candidate in "$@"; do
        ok=1
        for pkg in $candidate; do
            if ! apt-cache show "$pkg" >/dev/null 2>&1; then ok=0; break; fi
        done
        if [ "$ok" = 1 ]; then
            echo "package set available: $candidate"
            apt_install $candidate
            return 0
        fi
        echo "package set not on this image, trying the next: $candidate"
    done
    echo "::error::none of the candidate package sets exist: $*"
    return 1
}
