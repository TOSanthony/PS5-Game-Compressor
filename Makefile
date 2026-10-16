# Game Compressor - standalone PS5 payload build.

SHELL := bash

ifeq ($(strip $(PS5_PAYLOAD_SDK)),)
$(error PS5_PAYLOAD_SDK is required, e.g. export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

PYTHON ?= python3
LLVM_BINDIR ?= $(shell dirname "$$(command -v clang-18 2>/dev/null || command -v clang 2>/dev/null || command -v llvm-strip-18 2>/dev/null || command -v llvm-strip 2>/dev/null || echo clang)" 2>/dev/null || echo .)
export LLVM_BINDIR
export LLVM_CONFIG

BIN := game-compressor.elf
BUILD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
# Monotonic build number (the CI run number). 0 for local builds. The self-update
# module compares it with the one in the latest release's build-info.json.
BUILD_NUMBER ?= 0

C_SRCS := src/gc_main.c
C_SRCS += src/gc_websrv.c
C_SRCS += src/gc_api.c
C_SRCS += src/gc_job.c
C_SRCS += src/gc_diag.c
C_SRCS += src/gc_notify.c
C_SRCS += src/gc_power_guard.c
C_SRCS += src/gc_app_installer.c
C_SRCS += src/gc_shadowmount.c
C_SRCS += src/gc_size_cache.c
C_SRCS += src/gc_update.c
C_SRCS += src/gc_icon_thumb.c
C_SRCS += src/ampr_index.c
C_SRCS += src/asset.c
C_SRCS += src/pfs_compress.c
C_SRCS += src/pfs_decompress.c
C_SRCS += src/pfs_repair.c
C_SRCS += src/pfs_ampr_hotswap.c
C_SRCS += src/pfs_block_pipeline.c
C_SRCS += src/pfs_validate_hash.c
C_SRCS += src/miniz_tinfl.c

ASSETS := $(wildcard assets/*)
GEN_SRCS := $(patsubst assets/%,gen/assets/%,$(ASSETS:=.c))
APP_ASSETS := assets-app/param.json assets-app/icon0.png
OBJS := $(patsubst %.c,build/%.o,$(C_SRCS) $(GEN_SRCS))

CFLAGS_COMMON := -Os -Wall -Werror -Isrc
CFLAGS_COMMON += -ffunction-sections -fdata-sections -flto
CFLAGS_COMMON += -DGAME_COMPRESSOR_VERSION=\"$(BUILD_VERSION)\"
CFLAGS_COMMON += -DGAME_COMPRESSOR_PORT=5910
CFLAGS_COMMON += -DGC_BUILD_NUMBER=$(BUILD_NUMBER)
CFLAGS_COMMON += -DVERSION_TAG=\"game-compressor\"
CFLAGS_COMMON += -DBUILD_VERSION=\"$(BUILD_VERSION)\"
CFLAGS_COMMON += -DMINIZ_USE_UNALIGNED_LOADS_AND_STORES=1

FAST_SRCS := src/pfs_compress.c src/pfs_decompress.c src/pfs_repair.c src/pfs_ampr_hotswap.c src/pfs_block_pipeline.c src/pfs_validate_hash.c src/miniz_tinfl.c
FAST_OBJS := $(patsubst %.c,build/%.o,$(FAST_SRCS))

PFSC_ENCODER ?= runtime
PFSC_ZLIB_LEVEL ?= 7
PFSC_THRESHOLD_GAIN ?= 5
PFSC_FORCE_RAW_EXEC ?= 1
ZLIB_INCLUDE := third_party/zlib
ZLIB_LIB := build/zlib/libz.a
ZLIB_SRCS := adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c
ZLIB_HEADERS := $(wildcard third_party/zlib/*.h)
ZLIB_OBJS := $(patsubst %.c,build/zlib/%.o,$(ZLIB_SRCS))

ifneq ($(filter $(PFSC_ENCODER),runtime zlib miniz),)
CFLAGS_COMMON += -DGC_PFSC_ZLIB_LEVEL=$(PFSC_ZLIB_LEVEL)
CFLAGS_COMMON += -DGC_PFSC_THRESHOLD_GAIN=$(PFSC_THRESHOLD_GAIN)
CFLAGS_COMMON += -DGC_PFSC_FORCE_RAW_EXEC=$(PFSC_FORCE_RAW_EXEC)
CFLAGS_COMMON += -I$(ZLIB_INCLUDE)
else
$(error unsupported PFSC_ENCODER=$(PFSC_ENCODER), expected runtime, zlib, or miniz)
endif

FAST_CFLAGS := $(filter-out -Os,$(CFLAGS_COMMON)) -O2
LDFLAGS_COMMON := -Wl,--gc-sections -flto
LDADD := -lSceNotification -lSceSystemService
LDADD += $(ZLIB_LIB)
# libcurl + OpenSSL (static, built by tools/build-tls-deps.sh) for the self-update.
TLS_PREFIX := $(PS5_PAYLOAD_SDK)/target/user/homebrew
CFLAGS_COMMON += -I$(TLS_PREFIX)/include -Igen
LDADD += -L$(TLS_PREFIX)/lib -lcurl -lssl -lcrypto
CA_HEADER := gen/gc_ca.h
CA_BUNDLE ?= /etc/ssl/certs/ca-certificates.crt

all: $(BIN)

check-sdk-fw1360:
	$(PYTHON) tools/check_sdk_fw1360.py "$(PS5_PAYLOAD_SDK)"

gen/assets:
	mkdir -p $@

gen/assets/%.c: assets/% | gen/assets
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(FAST_OBJS): build/%.o: %.c Makefile
	mkdir -p $(dir $@)
	"$(CC)" $(FAST_CFLAGS) -c $< -o $@

build/src/gc_app_installer.o: $(APP_ASSETS)

$(CA_HEADER): $(CA_BUNDLE) tools/embed_ca.py
	mkdir -p gen
	$(PYTHON) tools/embed_ca.py $< > $@

build/src/gc_update.o: $(CA_HEADER)

build/zlib/%.o: third_party/zlib/%.c $(ZLIB_HEADERS) Makefile
	mkdir -p $(dir $@)
	"$(CC)" -O2 -I$(ZLIB_INCLUDE) -c $< -o $@

$(ZLIB_LIB): $(ZLIB_OBJS)
	"$(AR)" rcs $@ $(ZLIB_OBJS)

build/%.o: %.c Makefile
	mkdir -p $(dir $@)
	"$(CC)" $(CFLAGS_COMMON) -c $< -o $@

$(BIN): $(OBJS) $(APP_ASSETS) $(ZLIB_LIB) | check-sdk-fw1360
	"$(CC)" $(CFLAGS_COMMON) $(LDFLAGS_COMMON) -o $@ $(OBJS) $(LDADD)
	"$(STRIP)" --strip-all $@

clean:
	rm -rf build gen $(BIN)

.SECONDARY: $(GEN_SRCS)
.PHONY: all clean check-sdk-fw1360
