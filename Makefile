# GPort2X build.
#   make            host build (library, tools, tests)
#   make test       build and run every test (tests/run_tests.sh)
#   make aarch64    static aarch64 build of everything (aarch64-linux-gnu-gcc); runs under qemu-aarch64 binfmt
#   make test-aarch64  run the test suite on the aarch64 build (needs qemu-user binfmt)
#   make armhf      cross-compile check with arm-linux-gnueabihf-gcc (native engine); skipped if absent
#   make test-native  build the native engine (armhf-native) and run the unit tests, tests/sys and
#                   tests/native on it; needs an AArch64 kernel with 32-bit compat (an arm64 VM or device)
#   make clean
# Options: CC=... WERROR=1 SDL=0 (disable the optional SDL2 backend)

CC      ?= $(shell command -v gcc-13 >/dev/null 2>&1 && echo gcc-13 || echo cc)
BUILD   ?= build/host
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align
CPPFLAGS += -Iinclude -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -MMD -MP $(EXTRA_CPPFLAGS)
LDFLAGS ?=
LDLIBS  += -lm
ifeq ($(WERROR),1)
CFLAGS += -Werror
endif

# Optional SDL2 front end (the headless backends are always built).
# pkg-config or sdl2-config supply the flags; a host with only the runtime
# library (no libSDL2.so dev link) is linked against the versioned library.
SDL ?= 1
ifeq ($(SDL),1)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)
ifneq ($(SDL_LIBS),)
SDL_HDR_OK := $(shell echo '#include <SDL.h>' | $(CC) $(EXTRA_CPPFLAGS) $(SDL_CFLAGS) -x c -fsyntax-only - 2>/dev/null && echo yes)
ifneq ($(SDL_HDR_OK),yes)
SDL_LIBS :=
endif
endif
ifneq ($(SDL_LIBS),)
SDL_LINK_OK := $(shell echo 'int main(void){return 0;}' | $(CC) $(LDFLAGS) -x c - $(SDL_LIBS) -o /dev/null 2>/dev/null && echo yes)
ifneq ($(SDL_LINK_OK),yes)
SDL_LIBS := $(subst -lSDL2,-l:libSDL2-2.0.so.0,$(SDL_LIBS))
SDL_LINK_OK := $(shell echo 'int main(void){return 0;}' | $(CC) $(LDFLAGS) -x c - $(SDL_LIBS) -o /dev/null 2>/dev/null && echo yes)
endif
ifeq ($(SDL_LINK_OK),yes)
CPPFLAGS += -DGPORT2X_HAVE_SDL=1 $(SDL_CFLAGS)
LDLIBS   += $(SDL_LIBS)
endif
endif
endif

MODULE_SRCS := $(filter-out src/main/%,$(wildcard src/*/*.c))
MODULE_OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(MODULE_SRCS))
LIB         := $(BUILD)/libgport2x.a
MAIN_SRCS   := $(wildcard src/main/*.c)
BIN         := $(if $(MAIN_SRCS),$(BUILD)/gport2x,)

TEST_SRCS := $(wildcard tests/*/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRCS))
# Helper programs used by script tests (not run as tests themselves).
TOOL_SRCS := $(wildcard tests/*/tool_*.c)
TOOL_BINS := $(patsubst tests/%.c,$(BUILD)/tools/%,$(TOOL_SRCS))

.PHONY: all test aarch64 test-aarch64 armhf armhf-native armhf-native-sdl test-native clean
all: $(LIB) $(BIN) $(TEST_BINS) $(TOOL_BINS)

$(BUILD)/tools/%: tests/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MF $@.d $< $(LIB) $(LDLIBS) -o $@

$(LIB): $(MODULE_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/gport2x: $(MAIN_SRCS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MF $@.d $(MAIN_SRCS) $(LIB) $(LDLIBS) -o $@

$(BUILD)/tests/%: tests/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MF $@.d $< $(LIB) $(LDLIBS) -o $@

test: all
	@BUILD=$(BUILD) sh tests/run_tests.sh

# Static, so the binaries run under a qemu-user binfmt without a sysroot.
aarch64:
	@command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || { echo "aarch64: aarch64-linux-gnu-gcc not found, skipped"; exit 0; }
	$(MAKE) CC=aarch64-linux-gnu-gcc BUILD=build/aarch64 SDL=0 LDFLAGS=-static all

test-aarch64: aarch64
	@BUILD=build/aarch64 sh tests/run_tests.sh

armhf:
	@command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1 || { echo "armhf: arm-linux-gnueabihf-gcc not found, skipped (native engine not built)"; exit 0; }
	$(MAKE) CC=arm-linux-gnueabihf-gcc BUILD=build/armhf SDL=0 build/armhf/libgport2x.a

# The native engine's binary: static, linked above the guest range (0xE0000000)
# so the guest can own [0x8000, 0xC0000000) of the process (docs/NATIVE_ENGINE.md).
armhf-native:
	$(MAKE) CC=arm-linux-gnueabihf-gcc BUILD=build/armhf SDL=0 \
	    LDFLAGS="-static -no-pie -Wl,-Ttext-segment=0xE0000000" all

# The native engine's tests (the game tests of tests/game would run the
# interpreter, which an emulated VM makes too slow; tests/native runs the game).
NATIVE_TESTS ?= ^(aemu|cpu|dev|elf|fs|gmem)/|^tests/(sys|native)/
test-native: armhf-native
	@BUILD=build/armhf TEST_FILTER='$(NATIVE_TESTS)' TEST_TIMEOUT=$${TEST_TIMEOUT:-1800} sh tests/run_tests.sh

# The same with the SDL2 front end, linked dynamically against armhf SDL2
# (an arm64 Debian/Ubuntu with libsdl2-dev:armhf); for PortMaster devices.
# SDL_SYSROOT: a directory with the armhf libsdl2-dev and libsdl2-2.0-0
# packages extracted (dpkg-deb -x), when they cannot be installed alongside
# the host's own (multiarch conflicts); SDL2's other libraries are the
# device's, so they are left unresolved at link time.
SDL_SYSROOT ?=
armhf-native-sdl:
	PKG_CONFIG_SYSROOT_DIR=$(SDL_SYSROOT) \
	PKG_CONFIG_LIBDIR=$(SDL_SYSROOT)/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/lib/arm-linux-gnueabihf/pkgconfig \
	$(MAKE) CC=arm-linux-gnueabihf-gcc BUILD=build/armhf-sdl SDL=1 \
	    EXTRA_CPPFLAGS="-I$(SDL_SYSROOT)/usr/include/arm-linux-gnueabihf" \
	    LDFLAGS="-no-pie -Wl,-Ttext-segment=0xE0000000 -Wl,--allow-shlib-undefined" build/armhf-sdl/gport2x

# Header dependencies recorded by -MMD (objects, tests and tools).
-include $(MODULE_OBJS:.o=.d) $(TEST_BINS:=.d) $(TOOL_BINS:=.d) $(BIN:=.d)

clean:
	rm -rf build
