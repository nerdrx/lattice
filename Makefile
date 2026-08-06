# NxTakt — native Linux DAW
#
#   make            release build   -> build/nxtakt
#   make debug      -O0 -g3 + asserts
#   make run        build and launch
#   make clean
#
# Wayland is the primary window backend; X11 is kept as a runtime fallback.
# Force one with NXTAKT_BACKEND=wayland|x11 at run time.

CXX      ?= g++
CC       ?= gcc
BIN      := build/nxtakt
GEN      := build/gen

PKGS     := jack alsa sndfile samplerate gl x11 xcursor freetype2 fontconfig lilv-0
WL_PKGS  := wayland-client wayland-egl wayland-cursor egl xkbcommon

# ---- Wayland protocol discovery -------------------------------------------
# wayland-protocols proper if installed, otherwise Qt6 ships the same upstream
# XML, which is enough for wayland-scanner.
PROTO_ROOTS := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null) \
               /usr/share/wayland-protocols /usr/share/qt6/wayland/protocols
findproto = $(firstword $(foreach r,$(PROTO_ROOTS),$(shell test -d $(r) && find $(r) -name '$(1)' 2>/dev/null | head -1)))

XDG_SHELL_XML  := $(call findproto,xdg-shell.xml)
XDG_DECO_XML   := $(call findproto,xdg-decoration-unstable-v1.xml)
FRAC_SCALE_XML := $(call findproto,fractional-scale-v1.xml)
VIEWPORTER_XML := $(call findproto,viewporter.xml)
SCANNER        := $(shell command -v wayland-scanner 2>/dev/null)

HAVE_WAYLAND := 0
ifneq ($(SCANNER),)
ifneq ($(XDG_SHELL_XML),)
ifeq ($(shell pkg-config --exists $(WL_PKGS) && echo yes),yes)
HAVE_WAYLAND := 1
endif
endif
endif

PROTO_NAMES := xdg-shell
ifneq ($(XDG_DECO_XML),)
PROTO_NAMES += xdg-decoration-unstable-v1
endif
ifneq ($(FRAC_SCALE_XML),)
PROTO_NAMES += fractional-scale-v1
endif
ifneq ($(VIEWPORTER_XML),)
PROTO_NAMES += viewporter
endif

# ---- flags ----------------------------------------------------------------
ALL_PKGS := $(PKGS)
ifeq ($(HAVE_WAYLAND),1)
ALL_PKGS += $(WL_PKGS)
endif

PKG_CF   := $(shell pkg-config --cflags $(ALL_PKGS))
PKG_LD   := $(shell pkg-config --libs $(ALL_PKGS))

WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS := -std=c++20 -fno-math-errno $(WARN) $(PKG_CF) -I$(GEN) -Ivendor/clap/include -MMD -MP
CFLAGS   := -std=c11 -w $(PKG_CF) -I$(GEN) -MMD -MP
LDLIBS   := $(PKG_LD) -lpthread -lm -ldl

ifeq ($(HAVE_WAYLAND),1)
CXXFLAGS += -DLAT_HAVE_WAYLAND=1
ifneq ($(XDG_DECO_XML),)
CXXFLAGS += -DLAT_HAVE_XDG_DECORATION=1
endif
ifneq ($(FRAC_SCALE_XML),)
CXXFLAGS += -DLAT_HAVE_FRACTIONAL_SCALE=1
endif
ifneq ($(VIEWPORTER_XML),)
CXXFLAGS += -DLAT_HAVE_VIEWPORTER=1
endif
endif

ifeq ($(MAKECMDGOALS),debug)
  CXXFLAGS += -O0 -g3 -fno-omit-frame-pointer -DLAT_DEBUG=1
  CFLAGS   += -O0 -g3
else
  CXXFLAGS += -O2 -g -DNDEBUG
  CFLAGS   += -O2 -g
endif

# ---- sources --------------------------------------------------------------
SRC := $(shell find src -name '*.cpp' | sort)
ifneq ($(HAVE_WAYLAND),1)
SRC := $(filter-out src/ui/window_wayland.cpp,$(SRC))
endif
# src/daemon is a separate program with its own main(); it is built by the
# build/nxtaktd rule below and must never be swept into the GUI's link.
SRC := $(filter-out src/daemon/%,$(SRC))

OBJ := $(patsubst src/%.cpp,build/obj/%.o,$(SRC))

ifeq ($(HAVE_WAYLAND),1)
PROTO_H := $(foreach n,$(PROTO_NAMES),$(GEN)/$(n)-client-protocol.h)
PROTO_C := $(foreach n,$(PROTO_NAMES),$(GEN)/$(n)-protocol.c)
OBJ     += $(patsubst $(GEN)/%.c,build/obj/gen/%.o,$(PROTO_C))
endif

DEP := $(OBJ:.o=.d)

.PHONY: all debug run clean audio-only config

all: $(BIN)
debug: $(BIN)

config:
	@echo "wayland backend : $(HAVE_WAYLAND)"
	@echo "  scanner       : $(SCANNER)"
	@echo "  xdg-shell     : $(XDG_SHELL_XML)"
	@echo "  xdg-decoration: $(XDG_DECO_XML)"
	@echo "  fractional    : $(FRAC_SCALE_XML)"
	@echo "  viewporter    : $(VIEWPORTER_XML)"
	@echo "  protocols     : $(PROTO_NAMES)"

# ---- protocol codegen -----------------------------------------------------
$(GEN)/xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/xdg-shell-protocol.c: $(XDG_SHELL_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

$(GEN)/xdg-decoration-unstable-v1-client-protocol.h: $(XDG_DECO_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/xdg-decoration-unstable-v1-protocol.c: $(XDG_DECO_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

$(GEN)/fractional-scale-v1-client-protocol.h: $(FRAC_SCALE_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/fractional-scale-v1-protocol.c: $(FRAC_SCALE_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

$(GEN)/viewporter-client-protocol.h: $(VIEWPORTER_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/viewporter-protocol.c: $(VIEWPORTER_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

# ---- compile --------------------------------------------------------------
$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJ) -o $@ $(LDLIBS)
	@echo "  ->  $@"

build/obj/%.o: src/%.cpp $(PROTO_H)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/obj/gen/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

audio-only: $(patsubst src/%.cpp,build/obj/%.o,$(wildcard src/audio/*.cpp src/core/*.cpp))
	@echo "audio layer OK"

# ---- tools and tests ------------------------------------------------------
# Deliberately standalone: none of these link the GUI, so they run headless in
# CI and stay usable when the UI is mid-refactor.
CORE_SRC  := src/core/common.cpp src/core/project.cpp src/audio/sample.cpp src/audio/engine.cpp
TOOL_LIBS := $(shell pkg-config --libs sndfile samplerate lilv-0) -ldl -lpthread -lm
TOOL_CF   := -std=c++20 -O2 -w $(shell pkg-config --cflags sndfile samplerate lilv-0) -Ivendor/clap/include

.PHONY: tools test
tools: build/gen_demo build/render build/pitch_check build/plugin_scan

build/gen_demo: tools/gen_demo.cpp $(CORE_SRC)
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)
# render materialises a project's device chains, so unlike gen_demo it needs the
# plugin backends linked in.
build/render: tools/render.cpp $(CORE_SRC) src/plugin/host.cpp src/plugin/lv2_host.cpp \
              src/plugin/clap_host.cpp src/plugin/internal_devices.cpp
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)
build/pitch_check: tools/pitch_check.cpp
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)
build/plugin_scan: tools/plugin_scan.cpp src/plugin/host.cpp src/plugin/lv2_host.cpp src/plugin/clap_host.cpp src/plugin/internal_devices.cpp src/core/common.cpp
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)
build/engine_test: tests/engine_test.cpp src/audio/engine.cpp src/core/common.cpp
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)

# src/ipc is header-only and depends on libc alone, so this one deliberately
# does not use TOOL_CF/TOOL_LIBS: no sndfile, no lilv, and warnings left on.
# -lrt is only needed for shm_open on glibc < 2.34; harmless after.
IPC_CF := -std=c++20 -O2 $(WARN)
IPC_H  := src/ipc/shm.h src/ipc/pool.h src/ipc/control.h src/ipc/client.h
build/ipc_test: tests/ipc_test.cpp src/ipc/shm.h
	@mkdir -p build
	$(CXX) $(IPC_CF) $< -o $@ -lrt -lpthread

# The engine daemon: Engine + a backend + the control region + the plugin
# layer, and no GUI. Phase 3 is where src/plugin joins the link: the daemon
# owns every PluginInstance now, so it needs the backends (lilv for LV2, dl for
# CLAP) that phases 1 and 2 could do without. Still no GUI, no window system and
# no sndfile — nxtaktd renders, it does not decode or draw.
DAEMON_SRC := src/daemon/nxtaktd.cpp src/audio/engine.cpp src/audio/backend.cpp \
              src/core/common.cpp \
              src/plugin/host.cpp src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
              src/plugin/internal_devices.cpp
DAEMON_CF  := -std=c++20 -O2 $(WARN) -Ivendor/clap/include \
              $(shell pkg-config --cflags jack alsa lilv-0)
DAEMON_LD  := $(shell pkg-config --libs jack alsa lilv-0) -ldl -lrt -lpthread -lm

build/nxtaktd: $(DAEMON_SRC) $(IPC_H) src/audio/engine.h src/audio/backend.h src/plugin/host.h
	@mkdir -p build
	$(CXX) $(DAEMON_CF) $(DAEMON_SRC) -o $@ $(DAEMON_LD)

# daemon_test spawns ./build/nxtaktd, so the binary is a build dependency of
# the test rather than something the test is trusted to find.
build/daemon_test: tests/daemon_test.cpp $(IPC_H) src/audio/engine.h build/nxtaktd
	@mkdir -p build
	$(CXX) $(IPC_CF) $< -o $@ -lrt -lpthread

# The internal devices, exercised through the same PluginInstance contract every
# third-party plugin goes through. This had no target for a long time and its
# header said "built by hand" -- which meant several hundred assertions that CI
# had never once run, and a suite nobody runs is a suite that rots. It needs the
# plugin backends linked because host.cpp reaches into both of them.
build/internal_device_test: tests/internal_device_test.cpp src/plugin/host.cpp \
                            src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
                            src/plugin/internal_devices.cpp src/core/common.cpp
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(shell pkg-config --libs lilv-0) -ldl

# Full headless check: engine unit tests, then a real render that must not be
# silent, then a plugin scan.
test: build/engine_test build/ipc_test build/daemon_test build/internal_device_test \
      build/render build/gen_demo build/plugin_scan
	./build/engine_test
	./build/ipc_test
	./build/daemon_test
	./build/internal_device_test
	./build/gen_demo /tmp/nxtakt-selftest >/dev/null
	./build/render /tmp/nxtakt-selftest/demo.lattice /tmp/nxtakt-selftest/render.wav --scene 2 --bars 2
	./build/plugin_scan | tail -3
	@echo "ALL CHECKS PASSED"

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build/obj build/gen $(BIN)

-include $(DEP)
