# 6Copains for disting NT
#
#   make            hardware plug-in -> plugins/6copains.o
#   make test       native behaviour tests (clang, ASan/UBSan)
#   make lua-test   tests for the 6Copains Banks Lua script (Lua 5.4)
#   make render     native WAV renders of nb_monamie's patches -> renders/
#   make inspect    check the ARM object and list its undefined symbols
#   make dist       release zip -> dist/6copains-plugin.zip
#   make verify     test + lua-test + hardware + inspect
#   make profile    temporary on-device profiler build (same GUID)

PLUGIN   := 6copains
VERSION  ?= $(shell cat VERSION 2>/dev/null || echo dev)
API_DIR  ?= distingNT_API
SRC      := src/copains.cpp
HEADERS  := src/copains_engine.h src/copains_plugin.h
INCLUDES := -I$(API_DIR)/include -Isrc

# ARM toolchain: use arm-none-eabi-c++ from PATH, else the newest
# Arm GNU Toolchain installed under /Applications (gcc-arm-embedded cask).
ARM_PREFIX ?= $(shell if command -v arm-none-eabi-c++ >/dev/null 2>&1; then echo arm-none-eabi-; \
	else d=$$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi/bin 2>/dev/null | tail -1); \
	[ -n "$$d" ] && echo "$$d/arm-none-eabi-"; fi)
ARM_CXX     := $(ARM_PREFIX)c++
ARM_NM      := $(ARM_PREFIX)nm
ARM_READELF := $(ARM_PREFIX)readelf
ARM_SIZE    := $(ARM_PREFIX)size

ARM_FLAGS := -std=c++20 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb \
	-fPIC -fno-rtti -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fno-threadsafe-statics -O2 -Wall -Wextra -Werror -Wdouble-promotion

HOST_CXX   ?= clang++
HOST_FLAGS := -std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	-Wall -Wextra -Werror

LUA ?= $(shell command -v lua5.4 2>/dev/null || command -v lua 2>/dev/null || echo /opt/homebrew/opt/lua@5.4/bin/lua5.4)

TEST_SRC := tests/test_copains.cpp tests/nt_stubs.cpp
TEST_HDR := tests/fake_host.h

.PHONY: all hardware test lua-test render inspect dist verify clean profile

all: hardware

hardware: plugins/$(PLUGIN).o

plugins/$(PLUGIN).o: $(SRC) $(HEADERS)
	@if [ -z "$(ARM_PREFIX)" ]; then echo "arm-none-eabi toolchain not found (brew install --cask gcc-arm-embedded)" >&2; exit 1; fi
	@mkdir -p $(@D)
	$(ARM_CXX) $(ARM_FLAGS) $(INCLUDES) -c -o $@ $(SRC)
	@$(ARM_SIZE) -A $@ | awk '$$2 > 0'

# Temporary on-device profiler: same GUID, cycle counts on the display.
profile: $(SRC) $(HEADERS)
	@mkdir -p plugins
	$(ARM_CXX) $(ARM_FLAGS) -DCOPAINS_PROFILE $(INCLUDES) -c -o plugins/$(PLUGIN).o $(SRC)
	@echo "Built PROFILE build at plugins/$(PLUGIN).o - rebuild with 'make clean hardware' afterwards"

build/test_copains: $(SRC) $(HEADERS) $(TEST_SRC) $(TEST_HDR)
	@mkdir -p $(@D)
	$(HOST_CXX) $(HOST_FLAGS) $(INCLUDES) -Itests -o $@ $(SRC) $(TEST_SRC)

test: build/test_copains
	./build/test_copains

lua-test:
	cd lua && $(LUA) tests/test_6copains_banks.lua

build/render_demo: $(SRC) $(HEADERS) tools/render_demo.cpp tests/nt_stubs.cpp $(TEST_HDR)
	@mkdir -p $(@D)
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Werror $(INCLUDES) -Itests -o $@ $(SRC) tools/render_demo.cpp tests/nt_stubs.cpp

render: build/render_demo
	@mkdir -p renders
	./build/render_demo renders

inspect: plugins/$(PLUGIN).o
	@$(ARM_READELF) -h $< | grep -Eq 'Class:[[:space:]]+ELF32' || (echo "not ELF32" >&2; exit 1)
	@$(ARM_READELF) -h $< | grep -Eq 'little endian' || (echo "not little-endian" >&2; exit 1)
	@$(ARM_READELF) -h $< | grep -Eq 'Type:[[:space:]]+REL' || (echo "not relocatable" >&2; exit 1)
	@$(ARM_READELF) -h $< | grep -Eq 'Machine:[[:space:]]+ARM' || (echo "not ARM" >&2; exit 1)
	@$(ARM_NM) --defined-only $< | grep -Eq '[[:space:]]pluginEntry$$' || (echo "pluginEntry missing" >&2; exit 1)
	@echo "PASS: $< is a little-endian relocatable ARM object exporting pluginEntry"
	@echo "Undefined symbols (resolved by the disting NT firmware):"
	@$(ARM_NM) -u $<

# Release zip with the SD card layout nt_helper and the NT gallery install from.
# Only the .o: the gallery skips a zip that mixes plug-in types (.o and .lua),
# and reads the GUID from the .o. The fixed name lets later releases match the
# same gallery entry.
DIST_ZIP := dist/$(PLUGIN)-plugin.zip

dist: plugins/$(PLUGIN).o
	@rm -rf build/dist && mkdir -p build/dist/programs/plug-ins dist
	cp plugins/$(PLUGIN).o build/dist/programs/plug-ins/
	@rm -f $(DIST_ZIP)
	cd build/dist && zip -X -r ../../$(DIST_ZIP) programs
	@unzip -l $(DIST_ZIP)

verify: test lua-test hardware inspect

clean:
	rm -rf build plugins dist
