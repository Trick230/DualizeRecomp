DOLRECOMP_DIR    := lib/DolRecomp
MODERNGEKKO_DIR  := lib/ModernGekko
DOLRECOMP_BUILD  := $(DOLRECOMP_DIR)/build
MODERNGEKKO_BUILD:= $(MODERNGEKKO_DIR)/build

DOLRECOMP_BIN      := $(DOLRECOMP_BUILD)/dolrecomp
MODERNGEKKO_PORT_BIN := $(MODERNGEKKO_BUILD)/moderngekko-port

# One game per slug directory, so multiple extracted discs (GameCube or
# Wii, any title) can coexist. The slug is derived from ISO's filename the
# first time a game is set up; pass GAME=<slug> on later invocations to
# select it without ISO. There is no default game -- ISO= or GAME= must be
# given.
EXTRACTED_ROOT := extracted
GAME_SLUG := $(if $(ISO),$(strip $(shell basename "$(ISO)" | sed -E 's/\.[Ii][Ss][Oo]$$//; s/[^A-Za-z0-9]+/-/g; s/^-+//; s/-+$$//')),$(GAME))
EXTRACTED_DIR := $(EXTRACTED_ROOT)/$(GAME_SLUG)
MODULES_DIR   := build/modules

JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
CMAKE_BUILD_TYPE ?= Release
BUILD_TESTING ?= OFF

# Generated-code backend: portable C or native LLVM objects.
BACKEND ?= c
LLVM_DIR ?=
ifneq ($(filter $(BACKEND),c llvm),$(BACKEND))
$(error BACKEND must be c or llvm)
endif
DOLRECOMP_ENABLE_LLVM = $(if $(filter llvm,$(BACKEND)),ON,OFF)
LLVM_CMAKE_ARG = $(if $(and $(filter llvm,$(BACKEND)),$(LLVM_DIR)),-DLLVM_DIR="$(LLVM_DIR)")

# Toolchain used to compile the per-game module: auto, clang, gcc, or msvc.
# Auto follows the compiler used to build moderngekko-port. This keeps the
# generated module compatible with MSVC, MinGW GCC, and MinGW Clang builds.
# Clang is preferred when installed: moderngekko-port builds it with ThinLTO.
TOOLCHAIN ?= $(if $(shell command -v clang 2>/dev/null),clang,auto)

# Optimization level for the per-game module (0-3).
OPT_LEVEL ?= 3

# Games whose code partly ships in RSO modules get a load layout in
# config/rso/<disc id>.layout; with it the RSO code is recompiled too.
GAME_ID = $(shell head -c 6 "$(EXTRACTED_DIR)/sys/boot.bin" 2>/dev/null)
RSO_LAYOUT = $(wildcard config/rso/$(GAME_ID).layout)
# SDK functions with host implementations (C backend only) are listed in
# config/natives/<disc id>.natives.
NATIVES = $(if $(filter c,$(BACKEND)),$(wildcard config/natives/$(GAME_ID).natives))
PORT_ARGS = --backend $(BACKEND) --toolchain $(TOOLCHAIN) --opt-level $(OPT_LEVEL) --output $(MODULES_DIR) \
	$(if $(RSO_LAYOUT),--rso-layout $(RSO_LAYOUT)) $(if $(NATIVES),--natives $(NATIVES))

# Extra arguments forwarded after `--` to `moderngekko-run` via `make run`,
# e.g. RUN_ARGS="--headless" or RUN_ARGS="--graphics Vulkan".
RUN_ARGS ?=

WIT_STAMP := .git/.recomp-wit-stamp

# ModernGekko's user directory (same lookup as moderngekko-run). Per-game
# Dolphin settings from config/ are installed here before running.
# On Windows that is %LOCALAPPDATA% (CSIDL 28); cygpath asks the OS directly
# because MSYS make does not always inherit the variable.
LOCAL_APPDATA := $(shell cygpath -m -F 28 2>/dev/null)
ifneq ($(LOCAL_APPDATA),)
USER_DIR ?= $(LOCAL_APPDATA)/moderngekko
else
USER_DIR ?= $(or $(XDG_DATA_HOME),$(HOME)/.local/share)/moderngekko
endif

.DEFAULT_GOAL := help

.PHONY: help all check tools dolrecomp moderngekko submodules patches config wit extract \
        recompile run llvm llvm-run symbols clean clean-extracted clean-tools

help:
	@echo "DualizeRecomp - NiGHTS: Journey of Dreams static recompilation"
	@echo ""
	@echo "Works with any GameCube or Wii ISO -- there is no default game."
	@echo "ISO= or GAME= is required the first time you run a target that"
	@echo "needs an extracted game."
	@echo ""
	@echo "  make tools                       Build DolRecomp and ModernGekko"
	@echo "  make check                       Check tools, compiler, architecture, and sources"
	@echo "  make extract ISO=path/to.iso     Extract a GameCube/Wii ISO"
	@echo "  make recompile ISO=path/to.iso   Recompile + compile a runnable module"
	@echo "  make run ISO=path/to.iso         Recompile (if needed) and launch the game"
	@echo "  make llvm ISO=path/to.iso        Build an optimized LLVM object module"
	@echo "  make llvm-run ISO=path/to.iso    Build LLVM objects and launch the game"
	@echo "  make symbols GAME=<slug>         Identify SDK functions, write build/symbols/<slug>.map"
	@echo "  make clean                       Remove all build output"
	@echo ""
	@echo "  make run ISO=iso/Your\\ Game.iso   # first time for a new game"
	@echo "  make run GAME=Your-Game-Slug     # afterwards, once extracted"
	@echo ""
	@echo "TOOLCHAIN selects the compiler for the per-game module: auto, clang,"
	@echo "gcc, or msvc. The default, auto, follows the compiler used to"
	@echo "build ModernGekko. e.g. make run GAME=<slug> TOOLCHAIN=clang"
	@echo "BACKEND selects c or llvm. The llvm and llvm-run targets select llvm"
	@echo "automatically. Set LLVM_DIR if CMake cannot find LLVM 19 or 20."
	@echo ""
	@echo "ISO only needs to be passed once per game. It also picks the slug"
	@echo "used under extracted/<slug>/ -- pass GAME=<slug> on later invocations"
	@echo "instead of ISO to run a different already-extracted game."
	@echo ""
	@echo "Drop ISOs under iso/ (gitignored) if you want a fixed local path."
	@echo ""
	@echo "Bring your own legally-owned game dumps. No game data is included"
	@echo "in or downloaded by this repository."

all: tools

# --- build requirements -----------------------------------------------------

check:
	@sh scripts/check-build.sh $(if $(filter 1 yes true,$(FETCH)),--fetch) \
		$(if $(filter llvm,$(BACKEND)),--llvm)

# --- submodules -------------------------------------------------------------

submodules:
	@if [ -f "$(DOLRECOMP_DIR)/CMakeLists.txt" ] && \
	    [ -f "$(MODERNGEKKO_DIR)/CMakeLists.txt" ]; then \
		echo "ModernGekko and DolRecomp sources already present."; \
	else \
		git submodule sync --recursive; \
		git submodule update --init --recursive; \
	fi

# --- tools -------------------------------------------------------------------

dolrecomp: submodules
	cmake -S $(DOLRECOMP_DIR) -B $(DOLRECOMP_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
		-DBUILD_TESTING=$(BUILD_TESTING) \
		-DDOLRECOMP_ENABLE_LLVM=$(DOLRECOMP_ENABLE_LLVM) $(LLVM_CMAKE_ARG)
	cmake --build $(DOLRECOMP_BUILD) --target dolrecomp -j$(JOBS)

# Local fixes on top of the pinned submodules, stacked in order. Later patches
# can rewrite lines an earlier one added, so only the newest applied patch is
# guaranteed to reverse-apply: find it from the end and apply the rest.
# patches/ModernGekko/ targets the submodule itself, patches/ModernGekko-dolphin/
# its nested vendor/dolphin submodule (GXRuntime, the module CPU runtime) and
# patches/ModernGekko-dolphin-DolRecomp/ the recompiler vendored inside that.
PATCH_TARGETS := ModernGekko:$(MODERNGEKKO_DIR) ModernGekko-dolphin:$(MODERNGEKKO_DIR)/vendor/dolphin \
	ModernGekko-dolphin-DolRecomp:$(MODERNGEKKO_DIR)/vendor/dolphin/DolRecomp

patches: submodules
	@for target in $(PATCH_TARGETS); do \
		dir="$${target%%:*}"; repo="$${target#*:}"; \
		pending=""; \
		for patch in $$(ls -r patches/$$dir/*.patch 2>/dev/null); do \
			git -C "$$repo" apply --reverse --check "$(CURDIR)/$$patch" 2>/dev/null && break; \
			pending="$$patch $$pending"; \
		done; \
		for patch in $$pending; do \
			echo "Applying $$patch"; \
			git -C "$$repo" apply "$(CURDIR)/$$patch" || exit 1; \
		done; \
	done

# GameSettings are ours and always refreshed; the controller profile is only
# installed when the user has none, so local remaps are kept.
config:
	@mkdir -p "$(USER_DIR)/GameSettings" "$(USER_DIR)/Config"
	@cp config/GameSettings/*.ini "$(USER_DIR)/GameSettings/"
	@[ -f "$(USER_DIR)/Config/WiimoteNew.ini" ] || cp config/Config/WiimoteNew.ini "$(USER_DIR)/Config/"

moderngekko: patches
	cmake -S $(MODERNGEKKO_DIR) -B $(MODERNGEKKO_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
		-DBUILD_TESTING=$(BUILD_TESTING) \
		-DDOLRECOMP_ENABLE_LLVM=$(DOLRECOMP_ENABLE_LLVM) $(LLVM_CMAKE_ARG)
	cmake --build $(MODERNGEKKO_BUILD) --target moderngekko-port -j$(JOBS)

tools: dolrecomp moderngekko

# Wii ISO extraction needs Wiimms ISO Tools; DolRecomp downloads its own
# copy into extern/wit on first use. GameCube extraction doesn't need this,
# but running it unconditionally keeps the pipeline simple.
$(WIT_STAMP): | dolrecomp
	echo y | $(DOLRECOMP_BIN) --setup
	@if [ "$$(uname)" = "Darwin" ] && [ -d extern/wit/bin ]; then \
		codesign --force --deep --sign - extern/wit/bin/* 2>/dev/null || true; \
	fi
	@mkdir -p "$$(dirname "$(WIT_STAMP)")"
	@touch "$(WIT_STAMP)"

wit: $(WIT_STAMP)

# --- recompile pipeline -------------------------------------------------------

# Real file target: skipped automatically once this game has already been
# extracted, so ISO is only required the first time per game. dolrecomp/wit
# are order-only prerequisites (`|`) since they're phony/always "run" --
# normal prerequisites would force re-extraction on every invocation.
# There is no default game, so GAME_SLUG must come from ISO or GAME.
$(EXTRACTED_DIR)/sys/main.dol: | dolrecomp wit
	@if [ -z "$(GAME_SLUG)" ]; then \
		echo "error: no ISO= or GAME= given -- see 'make help'" >&2; \
		exit 1; \
	fi
	@if [ -z "$(ISO)" ]; then \
		echo "error: no extracted game at $(EXTRACTED_DIR) yet -- pass ISO=/path/to/game.iso" >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(ISO)" ]; then \
		echo "error: ISO not found: $(ISO)" >&2; \
		exit 1; \
	fi
	$(DOLRECOMP_BIN) extract "$(ISO)" $(EXTRACTED_DIR)
	@# Wii discs have multiple partitions (UPDATE/CHANNEL/DATA); the wit
	@# bridge extracts each into its own subfolder instead of flattening
	@# the game (DATA) partition to the top level like GameCube does.
	@if [ ! -f "$(EXTRACTED_DIR)/sys/main.dol" ] && [ -f "$(EXTRACTED_DIR)/DATA/sys/main.dol" ]; then \
		mv "$(EXTRACTED_DIR)/DATA"/* "$(EXTRACTED_DIR)/"; \
		rmdir "$(EXTRACTED_DIR)/DATA"; \
	fi

extract: $(EXTRACTED_DIR)/sys/main.dol

# moderngekko-port caches compiled modules by DOL hash + toolchain, so
# re-running these is cheap once a module has already been built.
recompile: moderngekko $(EXTRACTED_DIR)/sys/main.dol
	$(MODERNGEKKO_PORT_BIN) build $(EXTRACTED_DIR) $(PORT_ARGS)

run: moderngekko config $(EXTRACTED_DIR)/sys/main.dol
	$(MODERNGEKKO_PORT_BIN) run $(EXTRACTED_DIR) $(PORT_ARGS) -- $(RUN_ARGS)

# SDK function names found by matching Dolphin's signature database. The map
# is derived from the user's own DOL, so it is generated, never committed.
SYMBOL_DB  := $(MODERNGEKKO_DIR)/vendor/dolphin/Data/Sys/totaldb.dsy
SYMBOL_MAP := build/symbols/$(GAME_SLUG).map

$(SYMBOL_MAP): $(EXTRACTED_DIR)/sys/main.dol scripts/find_sdk_symbols.py | submodules
	@mkdir -p $(dir $@)
	python scripts/find_sdk_symbols.py $< $(SYMBOL_DB) --map $@ -o $(basename $@).txt

symbols: $(SYMBOL_MAP)

llvm: BACKEND=llvm
llvm: recompile

llvm-run: BACKEND=llvm
llvm-run: run

# --- cleanup -------------------------------------------------------------------

clean-extracted:
	rm -rf $(EXTRACTED_ROOT) generated $(MODULES_DIR)

clean-tools:
	rm -rf $(DOLRECOMP_BUILD) $(MODERNGEKKO_BUILD)

clean: clean-extracted clean-tools
