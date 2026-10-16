# Game Compressor TV app - build entry points.
# Adapted from the Orbit Store TV app, ps5-homebrew-ui and
# ps5-native-app-boilerplate (GPL-3.0-or-later). See NOTICE.md.
#
#   make GC_PAYLOAD=../game-compressor.elf ffpkg      (Ubuntu 24.04, .NET 8, clang-18)

SHELL := /bin/bash
.DEFAULT_GOAL := app

# The kit's build reads these from the environment.
APP_DEFINITIONS ?=
APP_INCLUDE_PATHS ?=
APP_STATIC_ARCHIVES ?=
APP_IMPORT_STUBS ?=
APP_RUNTIME_MODULES ?=
PS5_OPENGL_PREFIX ?=
OPENGL_SDK := .deps/ps5-opengl/current
override APP_DEFINITIONS := $(strip GL_GLEXT_PROTOTYPES=1 $(APP_DEFINITIONS))
override APP_INCLUDE_PATHS := $(strip src $(OPENGL_SDK)/include $(APP_INCLUDE_PATHS))
override APP_STATIC_ARCHIVES := $(strip .deps/ps5-opengl/libps5opengl-group.a $(APP_STATIC_ARCHIVES))
override APP_IMPORT_STUBS := $(strip $(OPENGL_SDK)/lib/libSceAgc.so \
	$(OPENGL_SDK)/lib/libSceAgcDriver.so $(APP_IMPORT_STUBS))
# No WebP needed: the app draws no downloaded artwork.
PACBREW_PACKAGES ?=
PACBREW_INCLUDE_PATHS ?=
PACBREW_STATIC_ARCHIVES ?=
BUILD_JOBS ?= $(shell nproc 2>/dev/null || echo 2)
USE_CCACHE ?= 0
export BUILD_JOBS USE_CCACHE
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_IMPORT_STUBS APP_RUNTIME_MODULES
export PS5_OPENGL_PREFIX PACBREW_PACKAGES PACBREW_INCLUDE_PATHS PACBREW_STATIC_ARCHIVES

# The Game Compressor payload, carried in the package so the app can start it
# when it isn't running (src/gc/starter.hpp). Build it first with `make` in the
# repository root, or pass GC_PAYLOAD=none for an app that only waits.
GC_PAYLOAD ?= ../game-compressor.elf
GC_VERSION ?= unknown
PAYLOAD_STAGE := build/package-extra
export APP_EXTRA_DIR := $(PAYLOAD_STAGE)

RUNTIME := runtime/libc.prx
RUNTIME_INPUTS := tools/rebuild-libc.sh tools/build-host-tools.sh tools/ninja-build.sh \
	$(wildcard tooling/native/*.cpp tooling/native/*.hpp) \
	$(wildcard tooling/native/runtime/*.txt)

.PHONY: app ffpkg ffpfsc packages deps opengl libc payload-stage clean distclean help

app: $(RUNTIME) opengl payload-stage
	@printf '%s\n' '==> [app] Compiling, linking, signing and assembling the app folder'
	@bash tools/build.sh Folder

ffpkg: $(RUNTIME) opengl payload-stage
	@printf '%s\n' '==> [ffpkg] Building the app folder and its UFS2 image'
	@bash tools/build.sh Ffpkg

ffpfsc: $(RUNTIME) opengl payload-stage
	@printf '%s\n' '==> [ffpfsc] Building the app folder and its compressed image'
	@bash tools/build.sh Ffpfsc

packages: $(RUNTIME) opengl payload-stage
	@bash tools/build.sh All

deps:
	@bash tools/setup-native-dependencies.sh
	@bash tools/prepare-opengl.sh

payload-stage:
	@bash tools/stage-payload.sh "$(GC_PAYLOAD)" "$(PAYLOAD_STAGE)" "$(GC_VERSION)"

opengl:
	@bash tools/prepare-opengl.sh

libc:
	@bash tools/rebuild-libc.sh

$(RUNTIME): $(RUNTIME_INPUTS)
	@printf '%s\n' '==> [libc] Generating the clean-room loader shim'
	@bash tools/rebuild-libc.sh

clean:
	@rm -rf -- build dist
	@rm -f -- $(RUNTIME)

distclean: clean
	@rm -rf -- .deps

help:
	@printf '%s\n' \
	  'make            Build the PS5 app folder in dist/PPSA99290' \
	  'make ffpkg      Also build dist/PPSA99290.ffpkg (ShadowMountPlus image)' \
	  'make GC_PAYLOAD=none ...   Build without bundling game-compressor.elf'
