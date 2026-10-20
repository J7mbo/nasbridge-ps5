# Builds the PS5 payload. Runs INSIDE the toolchain container; use the
# top-level Makefile (`make build`) rather than calling this directly.

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is undefined: run `make build` from the project root)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

VERSION  := $(shell cat VERSION)
BUILD_ID ?= local

BUILD_DIR := build/ps5
OUT_DIR   := out
INSTALLER := $(OUT_DIR)/installer.elf
HELPER    := $(OUT_DIR)/helper.elf
COPIER    := $(OUT_DIR)/copier.elf

INSTALLER_SRCS := $(wildcard tools/installer/*.c)
INSTALLER_OBJS := $(INSTALLER_SRCS:tools/installer/%.c=$(BUILD_DIR)/installer/%.o)

# The delete helper: shipped inside the app (assets/helper.elf); NASBridge
# sends it to the PS5's ELF loader when a game is deleted.
HELPER_SRCS := tools/helper/helper.c

# The copy helper: shipped inside the app (assets/copier.elf); NASBridge sends
# it to the PS5's ELF loader and hands it each file to copy (see copier.c).
COPIER_SRCS := tools/copier/copier.c

SELFTEST_DIR := $(BUILD_DIR)/selftest/NASBSELFTEST

# The UI kit (ps5-homebrew-ui, pinned in the image) and its prefetched
# dependencies. Both the app and `kit-check` build in private, writable copies
# of it under build/, so the kit itself is never modified.
KIT          := /opt/ps5-homebrew-ui
KIT_COMMIT   := $(shell cat $(KIT)/.nasbridge-commit 2>/dev/null)
STB          := /opt/stb
KIT_HOST_ENV := USE_CCACHE=0 HOST_CC=clang-18 HOST_CXX=clang++-18

# Home-screen app: the kit's app (renderer, shell, input, sound) with our
# screens (app/screens) in place of its demo designs, our overrides
# (app/overrides) copied over its files, and our sce_sys.
APP_WORK        := build/ui
TITLE_ID        := $(shell python3 -c 'import json;print(json.load(open("app/sce_sys/param.json"))["titleId"])')
APP_OUT         := $(OUT_DIR)/app/$(TITLE_ID)
# param.json wants NN.NNN.NNN: VERSION 0.3.0 becomes 00.003.000.
CONTENT_VERSION := $(shell python3 -c 'import sys;a,b,c=open("VERSION").read().strip().split(".");print(f"{int(a):02d}.{int(b):03d}.{int(c):03d}")')

# Reproducible output: the source tree is always mounted at /src, timestamps
# come from SOURCE_DATE_EPOCH, and no absolute build paths end up in the ELF.
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -g \
          -ffile-prefix-map=/src/=./ \
          -I$(BUILD_DIR)

.PHONY: all clean selftest test app app-work snapshots kit-work kit-check release FORCE

all: $(INSTALLER) selftest app

# Regenerated every build (FORCE) but only rewritten when VERSION or BUILD_ID
# actually changed, so a new git commit updates the ID without needless rebuilds.
$(BUILD_DIR)/version.h: FORCE | $(BUILD_DIR)
	@printf '#pragma once\n#define NASBRIDGE_VERSION "%s"\n#define NASBRIDGE_BUILD_ID "%s"\n' \
	  "$(VERSION)" "$(BUILD_ID)" > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

# Records which toolchain built the objects. The file only changes when the
# toolchain does, and every object depends on it, so upgrading the SDK or
# compiler rebuilds everything while unchanged builds stay instant.
$(BUILD_DIR)/toolchain.id: FORCE | $(BUILD_DIR)
	@cat /etc/nasbridge-toolchain > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD_DIR)/installer/%.o: tools/installer/%.c $(BUILD_DIR)/toolchain.id
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(INSTALLER): $(INSTALLER_OBJS) | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(INSTALLER_OBJS)
	@sha256sum $@

$(HELPER): $(HELPER_SRCS) $(BUILD_DIR)/toolchain.id | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(HELPER_SRCS)
	@sha256sum $@

$(COPIER): $(COPIER_SRCS) $(BUILD_DIR)/toolchain.id | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(COPIER_SRCS)
	@sha256sum $@

# ---- release: one ELF that installs the app ----
# out/NASBridge-<version>.elf is the installer (tools/installer) built with
# the app inside it: sent to the PS5's ELF loader, it installs or updates
# NASBridge on its own. The app folder is packed as the installer's own install
# stream, zlib-compressed (tools/release/pack.py), and linked in by
# tools/release/blob.S. zlib's decompressor comes from the source the UI kit
# already fetched into the image.
RELEASE      := $(OUT_DIR)/NASBridge-$(VERSION).elf
RELEASE_DIR  := $(BUILD_DIR)/release
ZLIB_SRC     := $(KIT)/.deps/native/zlib/zlib-1.3.2
ZLIB_OBJS    := $(patsubst %,$(RELEASE_DIR)/zlib/%.o,adler32 crc32 inffast inflate inftrees zutil)

# The app first, in a make of its own: the packing below reads what it built.
release:
	@$(MAKE) --no-print-directory -f ps5.mk app
	@$(MAKE) --no-print-directory -f ps5.mk $(RELEASE)

# Repacked every time (FORCE), but only rewritten when the app changed.
$(RELEASE_DIR)/app.nbz: FORCE
	@mkdir -p $(@D)
	@test -d $(APP_OUT) || { echo "no app in $(APP_OUT): run make -f ps5.mk app" >&2; exit 1; }
	@python3 tools/release/pack.py $(APP_OUT) $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(RELEASE_DIR)/zlib/%.o: $(ZLIB_SRC)/%.c $(BUILD_DIR)/toolchain.id
	@mkdir -p $(@D)
	$(CC) -O2 -w -ffile-prefix-map=$(ZLIB_SRC)/=zlib/ -I$(ZLIB_SRC) -c -o $@ $<

$(RELEASE_DIR)/blob.o: tools/release/blob.S $(RELEASE_DIR)/app.nbz
	$(CC) -DNB_BLOB='"$(RELEASE_DIR)/app.nbz"' -c -o $@ $<

$(RELEASE_DIR)/installer.o: tools/installer/installer.c tools/installer/sha256.h \
    $(BUILD_DIR)/version.h $(BUILD_DIR)/toolchain.id
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DINSTALLER_EMBEDDED -I$(ZLIB_SRC) -c -o $@ $<

$(RELEASE): $(RELEASE_DIR)/installer.o $(BUILD_DIR)/installer/sha256.o $(ZLIB_OBJS) \
    $(RELEASE_DIR)/blob.o | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $^
	@ls -l $@ | awk '{printf "%s: %.1f MB\n", $$NF, $$5 / 1e6}'
	@sha256sum $@

# A small, fixed folder for `make install-selftest`: nested folders, a name
# with spaces, and a 3 MB file so transfers span many chunks. It's written to
# /data/nasbridge/selftest on the PS5, never to the home-screen folder.
selftest:
	@rm -rf $(SELFTEST_DIR) && mkdir -p "$(SELFTEST_DIR)/sce_sys" "$(SELFTEST_DIR)/data/nested dir"
	@printf 'NASBridge installer self-test %s\n' "$(VERSION)" > $(SELFTEST_DIR)/README.txt
	@printf '{"test": true}\n' > $(SELFTEST_DIR)/sce_sys/param.json
	@head -c 3145728 /dev/zero | tr '\0' 'N' > "$(SELFTEST_DIR)/data/nested dir/big file.bin"

# A private, writable copy of the kit in $(1). Its prefetched dependencies stay
# in the read-only image and are linked in piece by piece, because the kit's
# build writes a small link next to the OpenGL SDK on every run. The copy is
# made again only when the kit's pinned commit changes.
define kit_copy
	@test -n "$(KIT_COMMIT)" || { echo "UI kit missing from the image: run make image" >&2; exit 1; }
	@if [ "$$(cat $(1)/.nasbridge-commit 2>/dev/null)" != "$(KIT_COMMIT)" ]; then \
	  echo "==> [kit] Preparing a copy of the UI kit in $(1) ($(KIT_COMMIT))"; \
	  rm -rf $(1) && mkdir -p $(1) && \
	  (cd $(KIT) && tar --exclude=./.git --exclude=./.deps --exclude=./build --exclude=./dist -cf - .) | tar -xf - -C $(1) && \
	  mkdir -p $(1)/.deps/ps5-opengl && \
	  ln -s $(KIT)/.deps/native $(1)/.deps/native && \
	  for sdk in $(KIT)/.deps/ps5-opengl/ps5-opengl-sdk-*; do ln -s $$sdk $(1)/.deps/ps5-opengl/; done; \
	fi
endef

# Puts our screens, overrides and sce_sys into the app's copy of the kit. cp -a
# keeps timestamps, so unchanged files don't rebuild. The kit's demo music and
# its Paper sound set are left out: NASBridge plays the Glass sounds only.
app-work: FORCE
	$(call kit_copy,$(APP_WORK))
	@rm -rf $(APP_WORK)/src/concepts && mkdir -p $(APP_WORK)/src/concepts
	@cp -a app/screens/. $(APP_WORK)/src/concepts/
	@# A kit file that an override replaced earlier, but no longer does, goes
	@# back to the kit's version (with a fresh timestamp, so it rebuilds).
	@cd $(KIT) && find src -type f ! -path 'src/concepts/*' | while read -r f; do \
	  [ -e "$(CURDIR)/app/overrides/$$f" ] || cmp -s "$$f" "$(CURDIR)/$(APP_WORK)/$$f" || \
	  cp "$$f" "$(CURDIR)/$(APP_WORK)/$$f"; done
	@cp -a app/overrides/. $(APP_WORK)/
	@rm -rf $(APP_WORK)/sce_sys && cp -a app/sce_sys $(APP_WORK)/sce_sys
	@python3 -c 'import json;p="$(APP_WORK)/sce_sys/param.json";d=json.load(open(p));d["contentVersion"]="$(CONTENT_VERSION)";open(p,"w").write(json.dumps(d,indent=2)+"\n")'
	@rm -f $(APP_WORK)/assets/audio/music/*.ogg
	@# Only the Glass sound set plays (NASBridge's design asks for it); the
	@# Paper set is 3 MB the app never uses.
	@rm -rf $(APP_WORK)/assets/audio/sfx/paper
	@test -f $(STB)/stb_image.h || { echo "stb_image missing from the image: run make image" >&2; exit 1; }
	@mkdir -p $(APP_WORK)/src/third_party/stb_image
	@cp -p $(STB)/stb_image.h $(STB)/LICENSE $(APP_WORK)/src/third_party/stb_image/

app: app-work $(HELPER) $(COPIER)
	@cp $(HELPER) $(APP_WORK)/assets/helper.elf
	@cp $(COPIER) $(APP_WORK)/assets/copier.elf
	@rm -rf $(APP_WORK)/dist/$(TITLE_ID)
	@echo "==> [app] Building $(TITLE_ID) $(CONTENT_VERSION)"
	@$(MAKE) --no-print-directory -C $(APP_WORK) app $(KIT_HOST_ENV) > $(APP_WORK)/build.log 2>&1 \
	  || { tail -60 $(APP_WORK)/build.log >&2; echo "app build failed (full log: $(APP_WORK)/build.log)" >&2; exit 1; }
	@test -f $(APP_WORK)/dist/$(TITLE_ID)/eboot.bin || { tail -60 $(APP_WORK)/build.log >&2; echo "app build produced no eboot.bin" >&2; exit 1; }
	@rm -rf $(OUT_DIR)/app && mkdir -p $(OUT_DIR)/app && cp -r $(APP_WORK)/dist/$(TITLE_ID) $(APP_OUT)
	@sha256sum $(APP_OUT)/eboot.bin
	@echo "App folder: $(APP_OUT)"

# Renders every screen's tour with the kit's PC preview (Mesa, no GPU) into
# out/snapshots: the same UI code and shaders as on the console.
snapshots: app-work
	@echo "==> [snapshots] Rendering NASBridge's screens"
	@rm -rf $(OUT_DIR)/snapshots
	@cd $(APP_WORK) && $(KIT_HOST_ENV) MESA_SHADER_CACHE_DISABLE=true bash tools/host-snapshots.sh $(CURDIR)/$(OUT_DIR)/snapshots all
	@echo "Pictures: $(OUT_DIR)/snapshots"

# ---------------------------------------------------------------------------
# UI kit check: builds the kit's own demo app (Homebrew UI Lab) for the PS5 and
# renders its Theme Lab screen with the PC preview, proving the kit's whole
# toolchain on this machine before our app depends on it.
KIT_WORK   := build/kit
KIT_LAB_ID := PPSA99050

kit-work: FORCE
	$(call kit_copy,$(KIT_WORK))

kit-check: kit-work
	@echo "==> [kit] Building Homebrew UI Lab for the PS5"
	@rm -rf $(KIT_WORK)/dist/$(KIT_LAB_ID)
	@$(MAKE) --no-print-directory -C $(KIT_WORK) app $(KIT_HOST_ENV) > $(KIT_WORK)/build.log 2>&1 \
	  || { tail -40 $(KIT_WORK)/build.log >&2; echo "kit PS5 build failed (log: $(KIT_WORK)/build.log)" >&2; exit 1; }
	@rm -rf $(OUT_DIR)/kit-lab && mkdir -p $(OUT_DIR)/kit-lab && cp -r $(KIT_WORK)/dist/$(KIT_LAB_ID) $(OUT_DIR)/kit-lab/
	@echo "PS5 app folder: $(OUT_DIR)/kit-lab/$(KIT_LAB_ID)"
	@echo "==> [kit] Rendering the Theme Lab screen with the PC preview"
	@rm -rf $(OUT_DIR)/kit-snapshots
	@cd $(KIT_WORK) && $(KIT_HOST_ENV) MESA_SHADER_CACHE_DISABLE=true bash tools/host-snapshots.sh $(CURDIR)/$(OUT_DIR)/kit-snapshots themes
	@echo "Pictures: $(OUT_DIR)/kit-snapshots"

$(BUILD_DIR) $(OUT_DIR):
	@mkdir -p $@

# Host build of the installer (short timeouts, writes under a temp folder) and
# the end-to-end tests that drive it through tools/install.py.
HOST_INSTALLER := build/host/installer

$(HOST_INSTALLER): $(INSTALLER_SRCS) tools/installer/sha256.h
	@mkdir -p $(@D)
	clang-18 -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE \
	  -DINSTALLER_HOST_TEST -DACCEPT_TIMEOUT_S=3 -DRECV_TIMEOUT_S=3 \
	  -o $@ $(INSTALLER_SRCS)

# ---- host tests ----
# Each source is compiled once into build/host/obj and kept: a test binary
# relinks from what is already built, and only changed files (or files whose
# headers changed) compile again. `make -j` builds and runs them side by side;
# each test talks to its own fake NAS ports.
HOST_OBJ := build/host/obj
HOST_INCLUDES := -DGL_GLEXT_PROTOTYPES=1 -I$(APP_WORK)/src -I$(APP_WORK)/.deps/ps5-opengl/current/include
host_objs = $(patsubst %.c,$(HOST_OBJ)/%.o,$(patsubst %.cpp,$(HOST_OBJ)/%.o,$(1)))

# Our code and the tests with warnings on; the kit's own files without.
$(HOST_OBJ)/$(APP_WORK)/src/concepts/%.o: $(APP_WORK)/src/concepts/%.cpp | app-work
	@mkdir -p $(@D)
	clang++-18 -std=c++20 -O1 -Wall -Wextra -MMD -MP $(HOST_INCLUDES) -c $< -o $@
$(HOST_OBJ)/tests/%.o: tests/%.cpp | app-work
	@mkdir -p $(@D)
	clang++-18 -std=c++20 -O1 -Wall -Wextra -MMD -MP $(HOST_INCLUDES) -c $< -o $@
$(HOST_OBJ)/%.o: %.cpp | app-work
	@mkdir -p $(@D)
	clang++-18 -std=c++20 -O1 -MMD -MP $(HOST_INCLUDES) -c $< -o $@
$(HOST_OBJ)/%.o: %.c | app-work
	@mkdir -p $(@D)
	clang-18 -O1 -w -MMD -MP -I$(APP_WORK)/src -c $< -o $@

# The kit's components, named where app-work puts them (it may not exist yet
# when this is read).
KIT_COMPONENTS := $(patsubst $(KIT)/%,$(APP_WORK)/%,$(wildcard $(KIT)/src/ui/components/*.cpp))
KIT_DRAWING := $(addprefix $(APP_WORK)/src/,core/save_file.cpp core/input.cpp gfx/draw_list.cpp \
  gfx/triangulate.cpp gfx/font.cpp ui/widgets.cpp ui/glyphs.cpp ui/theme.cpp ui/pixel_font.cpp)

# Copying and deleting games, against tests/fake_nas.py (a stand-in WebDAV NAS
# and ShadowMountPlus API on this computer).
HOST_TRANSFER := build/host/test_transfer
TRANSFER_SRCS := tests/test_transfer.cpp \
  $(addprefix $(APP_WORK)/src/concepts/,transfer.cpp copy_helper.cpp delete_helper.cpp file_calls.cpp smp.cpp \
    json_scan.cpp http.cpp connection.cpp library_cache.cpp notify.cpp storage.cpp) \
  $(APP_WORK)/src/core/save_file.cpp

# The folder picker's moves and listings (drawing isn't exercised, but the
# kit's drawing code is linked in so it builds as on the console).
HOST_PICKER := build/host/test_folder_picker
PICKER_SRCS := tests/test_folder_picker.cpp \
  $(addprefix $(APP_WORK)/src/concepts/,folder_picker.cpp http.cpp webdav_probe.cpp connection.cpp) \
  $(addprefix $(APP_WORK)/src/,core/save_file.cpp gfx/draw_list.cpp gfx/triangulate.cpp \
    gfx/font.cpp ui/widgets.cpp ui/glyphs.cpp ui/theme.cpp ui/pixel_font.cpp \
    ui/components/component.cpp)

# The connection test's game count, against what the library finds.
HOST_COUNT := build/host/test_game_count
COUNT_SRCS := tests/test_game_count.cpp \
  $(addprefix $(APP_WORK)/src/concepts/,webdav_probe.cpp http.cpp connection.cpp nas_library.cpp \
    library_cache.cpp exfat.cpp png.cpp) \
  $(APP_WORK)/src/core/save_file.cpp $(APP_WORK)/src/third_party/stb_image/stb_image.c

# The Connection screen's lock while a copy needs its settings.
HOST_LOCK := build/host/test_connection_lock
LOCK_SRCS := tests/test_connection_lock.cpp \
  $(addprefix $(APP_WORK)/src/concepts/,connection_page.cpp connection.cpp webdav_probe.cpp \
    http.cpp folder_picker.cpp) \
  $(KIT_DRAWING) $(KIT_COMPONENTS)

# The queue panel, driven by pad input over a real queue.
HOST_QUEUE := build/host/test_queue_panel
QUEUE_SRCS := tests/test_queue_panel.cpp \
  $(addprefix $(APP_WORK)/src/concepts/,queue_panel.cpp transfer.cpp copy_helper.cpp delete_helper.cpp \
    file_calls.cpp smp.cpp json_scan.cpp http.cpp connection.cpp library_cache.cpp notify.cpp \
    storage.cpp app_settings.cpp) \
  $(KIT_DRAWING) $(KIT_COMPONENTS)

# The delete helper (tools/helper/helper.c) and its client (delete_helper.cpp),
# driven through a stand-in ELF loader. HOST_HELPER is the helper built for this
# computer; the client sends it to the fake loader, which runs it.
HOST_HELPER := build/host/helper
# The copy helper (tools/copier/copier.c), built for this computer the same way;
# test_transfer drives it through the stand-in loader.
HOST_COPIER := build/host/copier
$(HOST_COPIER): tools/copier/copier.c
	@mkdir -p $(dir $@)
	clang-18 -std=c11 -Wall -Wextra -Werror -O1 -D_DEFAULT_SOURCE -DCOPIER_HOST_TEST -pthread -o $@ $<
$(HOST_HELPER): tools/helper/helper.c
	@mkdir -p $(@D)
	clang-18 -std=c11 -Wall -Wextra -Werror -O1 -D_DEFAULT_SOURCE -DHELPER_HOST_TEST -o $@ $<
HOST_DELETE := build/host/test_delete
DELETE_SRCS := tests/test_delete.cpp \
  $(APP_WORK)/src/concepts/delete_helper.cpp $(APP_WORK)/src/core/save_file.cpp

$(HOST_TRANSFER): $(call host_objs,$(TRANSFER_SRCS))
$(HOST_PICKER): $(call host_objs,$(PICKER_SRCS))
$(HOST_COUNT): $(call host_objs,$(COUNT_SRCS))
$(HOST_LOCK): $(call host_objs,$(LOCK_SRCS))
$(HOST_QUEUE): $(call host_objs,$(QUEUE_SRCS))
$(HOST_DELETE): $(call host_objs,$(DELETE_SRCS))
$(HOST_TRANSFER) $(HOST_PICKER) $(HOST_COUNT) $(HOST_LOCK) $(HOST_QUEUE) $(HOST_DELETE):
	@mkdir -p $(@D)
	clang++-18 -o $@ $^ -lpthread

-include $(shell find $(HOST_OBJ) -name '*.d' 2>/dev/null)

# Each suite is its own target, so `make -j` runs them at the same time.
# The transfer suite, the longest, runs as three parts at once (its own ports
# and work folder each): run-transfer runs all three.
TRANSFER_PARTS := run-transfer-1 run-transfer-2 run-transfer-3
HOST_RUNS := run-installer run-release $(TRANSFER_PARTS) run-picker run-count run-lock run-queue run-delete
.PHONY: $(HOST_RUNS) run-transfer
run-installer: selftest $(HOST_INSTALLER)
	python3 -u tests/test_installer.py
run-release:
	python3 -u tests/test_release.py
run-transfer: $(TRANSFER_PARTS)
$(TRANSFER_PARTS): run-transfer-%: $(HOST_TRANSFER) $(HOST_HELPER) $(HOST_COPIER)
	$(HOST_TRANSFER) build/host/transfer-test-$* $(HOST_HELPER) $(HOST_COPIER) $*
run-picker: $(HOST_PICKER)
	$(HOST_PICKER) build/host/picker-test
run-count: $(HOST_COUNT)
	$(HOST_COUNT) build/host/count-test
run-lock: $(HOST_LOCK)
	cd build/host && ./test_connection_lock
run-queue: $(HOST_QUEUE)
	$(HOST_QUEUE) build/host/queue-test
run-delete: $(HOST_DELETE) $(HOST_HELPER)
	$(HOST_DELETE) build/host/delete-test $(HOST_HELPER)

# app-work goes first, in a make of its own: it copies our sources into the
# kit, and a make that copied them while also building would judge the
# objects against the timestamps it saw before the copy (and keep stale ones).
test:
	@$(MAKE) --no-print-directory -f ps5.mk app-work
	@$(MAKE) --no-print-directory -f ps5.mk test-run
.PHONY: test-run
test-run: $(HOST_RUNS)

clean:
	rm -rf $(BUILD_DIR) build/host $(APP_WORK) $(KIT_WORK) $(INSTALLER) $(HELPER) $(COPIER) $(OUT_DIR)/NASBridge-*.elf $(OUT_DIR)/app $(OUT_DIR)/snapshots $(OUT_DIR)/kit-lab $(OUT_DIR)/kit-snapshots
