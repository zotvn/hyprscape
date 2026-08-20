# Plain build for distributions where `pkg-config hyprland` resolves out of the box.
# On NixOS use ./build.sh instead: it compiles inside the running Hyprland's own build env.

CXX      ?= g++
PKGS      = hyprland pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon
CXXFLAGS += -std=c++23 -shared -fPIC --no-gnu-unique -O2 \
            -Wall -Wno-narrowing -Wno-unused-parameter -Wno-unused-variable \
            $(shell pkg-config --cflags $(PKGS)) -Isrc
SRC       = $(shell find src -name '*.cpp' | sort)
OUT       = libhyprscape.so

all: $(OUT)

$(OUT): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f $(OUT)

.PHONY: all clean
