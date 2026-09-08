# hyprscape
#
#   make            build libhyprscape.so against the Hyprland `pkg-config` finds
#   make install    copy it to $(PREFIX)/lib  (default ~/.local)
#   make check      report what it would build against, and stop
#
# hyprpm calls `make clean && make all` with PKG_CONFIG_PATH already pointing at the headers for
# your running Hyprland, so nothing here may assume a system-wide install.
#
# On NixOS there is no system pkg-config entry for Hyprland at all -- use ./build.sh, which
# compiles inside the running Hyprland's own build environment, or the flake.

CXX          ?= g++
PKG_CONFIG   ?= pkg-config
PREFIX       ?= $(HOME)/.local
DESTDIR      ?=

OUT           = libhyprscape.so
SRC           = $(shell find src -name '*.cpp' | sort)

# `hyprland.pc` already Requires: aquamarine, hyprcursor, hyprgraphics, hyprlang, hyprutils,
# libdrm, egl, cairo, xkbcommon, libinput and wayland-server, and the chain reaches pixman and
# pango too on every distribution tested. Ask for those two by name anyway where pkg-config knows
# them -- belt and braces on a distro whose chain differs -- but never fail because it does not.
PKGS          = hyprland
PKGS         += $(shell $(PKG_CONFIG) --exists pixman-1 2>/dev/null && echo pixman-1)
PKGS         += $(shell $(PKG_CONFIG) --exists pangocairo 2>/dev/null && echo pangocairo)

HYPR_CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)

CXXFLAGS     += -std=c++23 -fPIC -O2 -Wall -Wno-narrowing -Wno-unused-parameter -Wno-unused-variable
CXXFLAGS     += $(HYPR_CFLAGS) -Isrc
LDFLAGS      += -shared

# --no-gnu-unique is not optional: STB_GNU_UNIQUE symbols pin a shared object into the process
# for good, so without it `hyprctl plugin unload` -- and every hyprpm update -- leaves the old
# copy loaded.
CXXFLAGS     += --no-gnu-unique

# The plugin hooks Hyprland by mangled symbol name, so it is tied to one release series. Building
# against anything else produces a plugin that loads and then cannot find what it needs.
SUPPORTED_HYPRLAND = 0.56
HYPRLAND_VERSION  := $(shell $(PKG_CONFIG) --modversion hyprland 2>/dev/null)
HYPRLAND_SERIES   := $(basename $(HYPRLAND_VERSION))

all: version-check $(OUT)

$(OUT): $(SRC)
	@echo "  CXX  $(OUT)  (Hyprland $(HYPRLAND_VERSION))"
	@$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRC)

version-check:
ifndef HYPRSCAPE_SKIP_VERSION_CHECK
	@if [ -z "$(HYPRLAND_VERSION)" ]; then \
	  echo ""; \
	  echo "  hyprscape: pkg-config cannot find Hyprland."; \
	  echo ""; \
	  echo "  Install your distribution's Hyprland headers, or build through hyprpm, which"; \
	  echo "  fetches headers matching your running compositor:"; \
	  echo ""; \
	  echo "      hyprpm add https://github.com/cybergaz/hyprscape"; \
	  echo ""; \
	  echo "  On NixOS, use ./build.sh or the flake instead."; \
	  echo ""; \
	  exit 1; \
	fi
	@if [ -z "$(HYPR_CFLAGS)" ]; then \
	  echo ""; \
	  echo "  hyprscape: found hyprland.pc (version $(HYPRLAND_VERSION)), but pkg-config could"; \
	  echo "  not resolve what it depends on. hyprland.pc Requires aquamarine, hyprcursor,"; \
	  echo "  hyprgraphics, hyprlang, hyprutils, libdrm, egl, cairo, xkbcommon, libinput and"; \
	  echo "  wayland-server -- one of those is missing, or PKG_CONFIG_PATH was replaced rather"; \
	  echo "  than appended to. pkg-config says:"; \
	  echo ""; \
	  $(PKG_CONFIG) --print-errors --cflags $(PKGS) 2>&1 | sed 's/^/      /'; \
	  echo ""; \
	  exit 1; \
	fi
	@if [ "$(HYPRLAND_SERIES)" != "$(SUPPORTED_HYPRLAND)" ]; then \
	  echo ""; \
	  echo "  hyprscape supports Hyprland $(SUPPORTED_HYPRLAND).x; found $(HYPRLAND_VERSION)."; \
	  echo ""; \
	  echo "  The plugin hooks the renderer by mangled C++ symbol, so a different release"; \
	  echo "  series will build and then fail at load. Check for a newer hyprscape first."; \
	  echo ""; \
	  echo "  To try anyway:  make HYPRSCAPE_SKIP_VERSION_CHECK=1"; \
	  echo ""; \
	  exit 1; \
	fi
endif

check:
	@echo "CXX               $(CXX)"
	@echo "Hyprland version  $(HYPRLAND_VERSION)"
	@echo "supported series  $(SUPPORTED_HYPRLAND).x"
	@echo "sources           $(words $(SRC)) files"
	@$(PKG_CONFIG) --cflags $(PKGS) >/dev/null && echo "pkg-config        ok"

install: all
	install -Dm755 $(OUT) $(DESTDIR)$(PREFIX)/lib/$(OUT)
	@echo ""
	@echo "  installed to $(DESTDIR)$(PREFIX)/lib/$(OUT)"
	@echo "  load it with:  hl.plugin.load(\"$(PREFIX)/lib/$(OUT)\")"
	@echo ""

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/$(OUT)

clean:
	rm -f $(OUT)

.PHONY: all check clean install uninstall version-check
