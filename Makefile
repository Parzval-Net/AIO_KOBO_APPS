# ==============================================================================
# Ink-boy for Kobo (Libra Colour MT8113T & Clara HD i.MX6ULL) — Build System
# ==============================================================================

HOST_CC      ?= clang
HOST_CXX     ?= clang++
PYTHON       ?= python3

# Auto-detect Kobo ARMhf cross-compiler:
# 1. Dedicated Kobo toolchain (arm-kobo-linux-gnueabihf-gcc)
# 2. Standard ARMhf toolchain (arm-linux-gnueabihf-gcc)
# 3. Zig cross-compiler (zig cc -target arm-linux-gnueabihf)
ifneq ($(wildcard ./tools/toolchain/zig),)
  ZIG_BIN  := ./tools/toolchain/zig
  KOBO_CC  := $(ZIG_BIN) cc -target arm-linux-musleabihf -mcpu=cortex_a7+neon -static
  KOBO_CXX := $(ZIG_BIN) c++ -target arm-linux-musleabihf -mcpu=cortex_a7+neon -static
  KOBO_ARCH_FLAGS :=
else ifneq ($(shell PATH="/opt/homebrew/bin:/usr/local/bin:$$PATH" which arm-kobo-linux-gnueabihf-gcc 2>/dev/null),)
  KOBO_CC  := arm-kobo-linux-gnueabihf-gcc
  KOBO_CXX := arm-kobo-linux-gnueabihf-g++
  KOBO_ARCH_FLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=hard
else ifneq ($(shell PATH="/opt/homebrew/bin:/usr/local/bin:$$PATH" which arm-linux-gnueabihf-gcc 2>/dev/null),)
  KOBO_CC  := arm-linux-gnueabihf-gcc
  KOBO_CXX := arm-linux-gnueabihf-g++
  KOBO_ARCH_FLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=hard
else ifneq ($(shell PATH="/opt/homebrew/bin:/usr/local/bin:$$PATH" which zig 2>/dev/null),)
  ZIG_BIN  := $(shell PATH="/opt/homebrew/bin:/usr/local/bin:$$PATH" which zig)
  KOBO_CC  := $(ZIG_BIN) cc -target arm-linux-musleabihf -mcpu=cortex_a7+neon -static
  KOBO_CXX := $(ZIG_BIN) c++ -target arm-linux-musleabihf -mcpu=cortex_a7+neon -static
  KOBO_ARCH_FLAGS :=
endif

INCLUDES := -Iinclude -Ilib/PeanutGB/src -Ilib/WalnutCGB/src
COMMON_WARN := -Wall -Wextra -Wno-unused-parameter

HOST_CFLAGS   := -O3 $(COMMON_WARN) $(INCLUDES)
HOST_CXXFLAGS := -O3 -std=c++17 $(COMMON_WARN) $(INCLUDES)
HOST_LDFLAGS  := -lpthread -lm

FBINK_INCLUDES := -Ilib/FBInk -Ilib/FBInk/i2c-tools/include
FBINK_DEFS     := -D_GNU_SOURCE -DHAVE_FBINK -DFBINK_FOR_KOBO -DFBINK_MINIMAL -DFBINK_WITH_DRAW -DFBINK_WITH_BITMAP -DFBINK_WITH_IMAGE

KOBO_CFLAGS   := -O3 $(KOBO_ARCH_FLAGS) -ffast-math -fdata-sections -ffunction-sections $(COMMON_WARN) -Wno-unknown-warning-option $(INCLUDES) $(FBINK_INCLUDES) $(FBINK_DEFS)
KOBO_CXXFLAGS := $(KOBO_CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
KOBO_LDFLAGS  := -Wl,--gc-sections -lpthread -lm

CORE_C_SRCS := \
	src/core/peanut_gb_impl.c \
	src/core/walnut_cgb_impl.c

CORE_CPP_SRCS := \
	src/core/emulator_core.cpp \
	src/platform/kobo_display.cpp \
	src/platform/kobo_input.cpp \
	src/platform/kobo_system.cpp

BUILD_DIR := build
DIST_DIR  := dist/Kobo-InkBoy

.PHONY: all host test aio-test aio demo-roms kobo bundle auto-install installer-web clean

all: host demo-roms test

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

demo-roms:
	@$(PYTHON) tools/generate_demo_roms.py $(DIST_DIR)/.adds/inkboy/roms

host: $(BUILD_DIR) demo-roms
	@echo "[HOST] Compiling macOS simulator binary -> $(BUILD_DIR)/inkboy-sim"
	$(HOST_CC) $(HOST_CFLAGS) -c src/core/peanut_gb_impl.c -o $(BUILD_DIR)/peanut_gb_impl.o
	$(HOST_CC) $(HOST_CFLAGS) -c src/core/walnut_cgb_impl.c -o $(BUILD_DIR)/walnut_cgb_impl.o
	$(HOST_CXX) $(HOST_CXXFLAGS) $(BUILD_DIR)/peanut_gb_impl.o $(BUILD_DIR)/walnut_cgb_impl.o \
		$(CORE_CPP_SRCS) src/app/main.cpp $(HOST_LDFLAGS) -o $(BUILD_DIR)/inkboy-sim

test: host
	@echo "[TEST] Compiling and running Kobo REA test suite..."
	$(HOST_CXX) $(HOST_CXXFLAGS) $(BUILD_DIR)/peanut_gb_impl.o $(BUILD_DIR)/walnut_cgb_impl.o \
		$(CORE_CPP_SRCS) tests/kobo_port_tests.cpp $(HOST_LDFLAGS) -o $(BUILD_DIR)/kobo_port_tests
	@$(BUILD_DIR)/kobo_port_tests
	@$(PYTHON) tools/kobo_auto_installer.py --self-test
	@$(PYTHON) -m unittest discover -s tests -p 'test_kobo_aio.py' 2>&1 | tail -1
	@echo "[SIM] Running 30 frames of GBC Color Demo on simulated Kobo Libra Colour & Clara HD..."
	@$(BUILD_DIR)/inkboy-sim --device libra --mode quality --frames 30 \
		--screenshot $(BUILD_DIR)/sim_libra_colour_kaleido3.ppm \
		$(DIST_DIR)/.adds/inkboy/roms/Kobo_Kaleido_Color_Demo.gbc
	@$(BUILD_DIR)/inkboy-sim --device clara --mode quality --frames 30 \
		--screenshot $(BUILD_DIR)/sim_clara_hd_carta4gray.ppm \
		$(DIST_DIR)/.adds/inkboy/roms/Kobo_Carta_Classic_Demo.gb
	@$(BUILD_DIR)/inkboy-sim --device clara --mode turbo --frames 30 \
		--screenshot $(BUILD_DIR)/sim_clara_hd_turbo_a2.ppm \
		$(DIST_DIR)/.adds/inkboy/roms/Kobo_Kaleido_Color_Demo.gbc

aio-test:
	@echo "[AIO] Running Kobo AIO test suite..."
	@$(PYTHON) -m unittest discover -s tests -p 'test_kobo_aio.py'

aio:
	@cd tools && $(PYTHON) -m kobo_aio serve

kobo: $(BUILD_DIR) demo-roms
ifdef KOBO_CC
	@echo "[KOBO] Cross-compiling ARMv7 hard-float binary with real FBInk -> $(DIST_DIR)/.adds/inkboy/inkboy"
	$(KOBO_CC) $(KOBO_CFLAGS) -c lib/FBInk/fbink.c -o $(BUILD_DIR)/kobo_fbink.o
	$(KOBO_CC) $(KOBO_CFLAGS) -c lib/FBInk/cutef8/utf8.c -o $(BUILD_DIR)/kobo_utf8.o
	$(KOBO_CC) $(KOBO_CFLAGS) -c lib/FBInk/cutef8/dfa.c -o $(BUILD_DIR)/kobo_dfa.o
	$(KOBO_CC) $(KOBO_CFLAGS) -c lib/FBInk/qimagescale/qimagescale.c -o $(BUILD_DIR)/kobo_qimagescale.o
	$(KOBO_CC) $(KOBO_CFLAGS) -c lib/FBInk/i2c-tools/lib/smbus.c -o $(BUILD_DIR)/kobo_smbus.o
	$(KOBO_CC) $(KOBO_CFLAGS) -c src/core/peanut_gb_impl.c -o $(BUILD_DIR)/kobo_peanut.o
	$(KOBO_CC) $(KOBO_CFLAGS) -c src/core/walnut_cgb_impl.c -o $(BUILD_DIR)/kobo_walnut.o
	$(KOBO_CXX) $(KOBO_CXXFLAGS) \
		$(BUILD_DIR)/kobo_fbink.o $(BUILD_DIR)/kobo_utf8.o $(BUILD_DIR)/kobo_dfa.o \
		$(BUILD_DIR)/kobo_qimagescale.o $(BUILD_DIR)/kobo_smbus.o \
		$(BUILD_DIR)/kobo_peanut.o $(BUILD_DIR)/kobo_walnut.o \
		$(CORE_CPP_SRCS) src/app/main.cpp $(KOBO_LDFLAGS) -o $(DIST_DIR)/.adds/inkboy/inkboy
	@chmod +x $(DIST_DIR)/.adds/inkboy/inkboy $(DIST_DIR)/.adds/inkboy/inkboy.sh
	@file $(DIST_DIR)/.adds/inkboy/inkboy
else
	@echo "[WARN] No ARMhf cross-compiler found. Install zig ('brew install zig') or arm-kobo-linux-gnueabihf-gcc."
endif

bundle: kobo
	@rm -f dist/Ink-boy-Kobo-Bundle.zip
	@cd $(DIST_DIR) && zip -r ../Ink-boy-Kobo-Bundle.zip .adds -x '*.sav'
	@echo "[BUNDLE] Created dist/Ink-boy-Kobo-Bundle.zip"

auto-install:
	@$(PYTHON) tools/kobo_auto_installer.py --watch --eject

installer-web:
	@$(PYTHON) tools/serve_installer.py 8765

clean:
	rm -rf $(BUILD_DIR) dist/Ink-boy-Kobo-Bundle.zip
