#!/usr/bin/env bash
# Builds and runs tests/timesig_view_test.cpp.
#
# It has no `make` target because the wave that wrote it does not own the
# Makefile. The line it is owed, beside build/internal_device_test:
#
#   build/timesig_view_test: tests/timesig_view_test.cpp src/audio/engine.cpp \
#                            src/core/common.cpp src/audio/sample.cpp \
#                            src/plugin/host.cpp src/plugin/lv2_host.cpp \
#                            src/plugin/clap_host.cpp src/plugin/internal_devices.cpp
#   	@mkdir -p build
#   	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS) -ldl
#
# ...and `build/timesig_view_test` added to the `test:` prerequisites and body.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
${CXX:-g++} -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    $(pkg-config --cflags sndfile samplerate lilv-0) -Ivendor/clap/include \
    tests/timesig_view_test.cpp \
    src/audio/engine.cpp src/audio/sample.cpp src/core/common.cpp \
    src/plugin/host.cpp src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
    src/plugin/internal_devices.cpp \
    -o build/timesig_view_test \
    $(pkg-config --libs sndfile samplerate lilv-0) -ldl -lm -lpthread
exec ./build/timesig_view_test
