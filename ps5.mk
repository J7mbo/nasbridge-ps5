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
# It copies over SMB with libsmb2 (below), built for it from the same sources
# and config.h as the app's.
COPIER_SRCS := tools/copier/copier.c

SELFTEST_DIR := $(BUILD_DIR)/selftest/NASBSELFTEST

# The UI kit (ps5-homebrew-ui, pinned in the image) and its prefetched
# dependencies. Both the app and `kit-check` build in private, writable copies
# of it under build/, so the kit itself is never modified.
KIT          := /opt/ps5-homebrew-ui
KIT_COMMIT   := $(shell cat $(KIT)/.nasbridge-commit 2>/dev/null)
STB          := /opt/stb
KIT_HOST_ENV := USE_CCACHE=0 HOST_CC=clang-18 HOST_CXX=clang++-18

# libsmb2 (pinned in the image): its library sources (lib/CMakeLists.txt's
# list, without Kerberos) are built into the app, with our config.h, address
# lookup and AES (app/third_party/libsmb2; aes_fast.c stands in for lib/aes.c). The kit compiles every source
# under src/, so they're copied into src/third_party/libsmb2, and the kit is
# told where the headers are.
LIBSMB2      := /opt/libsmb2
LIBSMB2_SRCS := aes128ccm.c alloc.c asn1-ber.c compat.c dcerpc.c dcerpc-lsa.c \
  dcerpc-srvsvc.c errors.c hmac.c hmac-md5.c init.c libsmb2.c md4c.c md5.c ntlmssp.c pdu.c \
  sha1.c sha224-256.c sha384-512.c smb2-cmd-close.c smb2-cmd-create.c smb2-cmd-echo.c \
  smb2-cmd-error.c smb2-cmd-flush.c smb2-cmd-ioctl.c smb2-cmd-lock.c smb2-cmd-logoff.c \
  smb2-cmd-negotiate.c smb2-cmd-notify-change.c smb2-cmd-oplock-break.c \
  smb2-cmd-query-directory.c smb2-cmd-query-info.c smb2-cmd-read.c smb2-cmd-session-setup.c \
  smb2-cmd-set-info.c smb2-cmd-tree-connect.c smb2-cmd-tree-disconnect.c smb2-cmd-write.c \
  smb2-data-file-info.c smb2-data-filesystem-info.c smb2-data-security-descriptor.c \
  smb2-data-reparse-point.c smb2-share-enum.c smb3-seal.c smb2-signing.c socket.c \
  spnego-wrapper.c sync.c timestamps.c unicode.c usha.c
LIBSMB2_OURS := smb2_compat.c aes_fast.c
LIBSMB2_DIR  := src/third_party/libsmb2
LIBSMB2_INCLUDES := $(LIBSMB2_DIR) $(LIBSMB2_DIR)/include $(LIBSMB2_DIR)/include/smb2
APP_ENV      := APP_DEFINITIONS=HAVE_CONFIG_H APP_INCLUDE_PATHS="$(LIBSMB2_INCLUDES)"
# The copy helper's libsmb2: the payload SDK's compiler, warnings off (it isn't
# our code), the image's paths kept out of the ELF.
COPIER_SMB_OBJS := $(patsubst %.c,$(BUILD_DIR)/libsmb2/%.o,$(LIBSMB2_SRCS) $(LIBSMB2_OURS))
COPIER_SMB_FLAGS := -std=c11 -O2 -g -w -DHAVE_CONFIG_H -Iapp/third_party/libsmb2 \
  -I$(LIBSMB2)/include -I$(LIBSMB2)/include/smb2 -I$(LIBSMB2)/lib \
  -ffile-prefix-map=$(LIBSMB2)/=libsmb2/ -ffile-prefix-map=/src/=./
COPIER_INCLUDES := -I$(LIBSMB2)/include

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

$(BUILD_DIR)/libsmb2/%.o: $(LIBSMB2)/lib/%.c app/third_party/libsmb2/config.h $(BUILD_DIR)/toolchain.id
	@mkdir -p $(@D)
	@$(CC) $(COPIER_SMB_FLAGS) -c -o $@ $<
$(BUILD_DIR)/libsmb2/%.o: app/third_party/libsmb2/%.c app/third_party/libsmb2/config.h $(BUILD_DIR)/toolchain.id
	@mkdir -p $(@D)
	@$(CC) $(COPIER_SMB_FLAGS) -c -o $@ $<

$(COPIER): $(COPIER_SRCS) $(COPIER_SMB_OBJS) $(BUILD_DIR)/toolchain.id | $(OUT_DIR)
	$(CC) $(CFLAGS) $(COPIER_INCLUDES) -o $@ $(COPIER_SRCS) $(COPIER_SMB_OBJS)
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
	@test -f $(LIBSMB2)/lib/libsmb2.c || { echo "libsmb2 missing from the image: run make image" >&2; exit 1; }
	@rm -rf $(APP_WORK)/$(LIBSMB2_DIR) && mkdir -p $(APP_WORK)/$(LIBSMB2_DIR)/lib
	@cp -p $(addprefix $(LIBSMB2)/lib/,$(LIBSMB2_SRCS)) $(LIBSMB2)/lib/*.h $(APP_WORK)/$(LIBSMB2_DIR)/lib/
	@cp -pR $(LIBSMB2)/include $(LIBSMB2)/COPYING $(LIBSMB2)/LICENCE-LGPL-2.1.txt $(APP_WORK)/$(LIBSMB2_DIR)/
	@cp -p app/third_party/libsmb2/* $(APP_WORK)/$(LIBSMB2_DIR)/

app: app-work $(HELPER) $(COPIER)
	@cp $(HELPER) $(APP_WORK)/assets/helper.elf
	@cp $(COPIER) $(APP_WORK)/assets/copier.elf
	@rm -rf $(APP_WORK)/dist/$(TITLE_ID)
	@echo "==> [app] Building $(TITLE_ID) $(CONTENT_VERSION)"
	@$(MAKE) --no-print-directory -C $(APP_WORK) app $(KIT_HOST_ENV) $(APP_ENV) > $(APP_WORK)/build.log 2>&1 \
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
	@# The kit's snapshot build takes no include paths or definitions:
	@# libsmb2's headers come through CPATH, and its HAVE_CONFIG_H (and the
	@# GNU C it's written in, over the kit's -std=c11) through a compiler
	@# that adds them.
	@mkdir -p $(APP_WORK)/build && printf '#!/bin/sh\nexec clang-18 -DHAVE_CONFIG_H "$$@" -std=gnu11\n' > $(APP_WORK)/build/snapshot-cc && chmod +x $(APP_WORK)/build/snapshot-cc
	@cd $(APP_WORK) && $(KIT_HOST_ENV) HOST_CC=$(CURDIR)/$(APP_WORK)/build/snapshot-cc \
	  CPATH=$(subst $(subst ,, ),:,$(addprefix $(CURDIR)/$(APP_WORK)/,$(LIBSMB2_INCLUDES))) \
	  MESA_SHADER_CACHE_DISABLE=true bash tools/host-snapshots.sh $(CURDIR)/$(OUT_DIR)/snapshots all
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
HOST_INCLUDES := -DGL_GLEXT_PROTOTYPES=1 -I$(APP_WORK)/src -I$(APP_WORK)/.deps/ps5-opengl/current/include \
  -I$(APP_WORK)/$(LIBSMB2_DIR)/include
host_objs = $(patsubst %.c,$(HOST_OBJ)/%.o,$(patsubst %.cpp,$(HOST_OBJ)/%.o,$(1)))

# libsmb2 for this computer, built as for the PS5 (same config.h), and our
# SMB layer over it: linked into every test that reaches the NAS over SMB.
LIBSMB2_HOST := $(APP_WORK)/src/concepts/smb.cpp $(addprefix $(APP_WORK)/$(LIBSMB2_DIR)/,$(LIBSMB2_OURS)) \
  $(addprefix $(APP_WORK)/$(LIBSMB2_DIR)/lib/,$(LIBSMB2_SRCS))

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
$(HOST_OBJ)/$(APP_WORK)/$(LIBSMB2_DIR)/%.o: $(APP_WORK)/$(LIBSMB2_DIR)/%.c | app-work
	@mkdir -p $(@D)
	clang-18 -O1 -w -MMD -MP -DHAVE_CONFIG_H $(addprefix -I$(APP_WORK)/,$(LIBSMB2_INCLUDES)) -c $< -o $@
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
TRANSFER_SRCS := tests/test_transfer.cpp $(LIBSMB2_HOST) \
  $(addprefix $(APP_WORK)/src/concepts/,transfer.cpp copy_helper.cpp delete_helper.cpp file_calls.cpp smp.cpp \
    json_scan.cpp http.cpp connection.cpp library_cache.cpp notify.cpp storage.cpp nas.cpp \
    nas_probe.cpp) \
  $(APP_WORK)/src/core/save_file.cpp

# The folder picker's moves and listings (drawing isn't exercised, but the
# kit's drawing code is linked in so it builds as on the console).
HOST_PICKER := build/host/test_folder_picker
PICKER_SRCS := tests/test_folder_picker.cpp $(LIBSMB2_HOST) \
  $(addprefix $(APP_WORK)/src/concepts/,folder_picker.cpp http.cpp nas_probe.cpp connection.cpp) \
  $(addprefix $(APP_WORK)/src/,core/save_file.cpp gfx/draw_list.cpp gfx/triangulate.cpp \
    gfx/font.cpp ui/widgets.cpp ui/glyphs.cpp ui/theme.cpp ui/pixel_font.cpp \
    ui/components/component.cpp)

# The connection test's game count, against what the library finds.
HOST_COUNT := build/host/test_game_count
COUNT_SRCS := tests/test_game_count.cpp $(LIBSMB2_HOST) \
  $(addprefix $(APP_WORK)/src/concepts/,nas_probe.cpp http.cpp connection.cpp nas_library.cpp \
    library_cache.cpp exfat.cpp pfs.cpp ufs2.cpp ffpfsc.cpp png.cpp nas.cpp) \
  $(APP_WORK)/src/core/save_file.cpp $(APP_WORK)/src/third_party/stb_image/stb_image.c

# Compressed images (.ffpfsc): the readers on images made by MkPFS and Dump
# Forge (tests/fixtures, unpacked first), damaged ones, and the library.
HOST_FFPFSC := build/host/test_ffpfsc
FFPFSC_SRCS := tests/test_ffpfsc.cpp $(LIBSMB2_HOST) \
  $(addprefix $(APP_WORK)/src/concepts/,nas_probe.cpp http.cpp connection.cpp nas_library.cpp \
    library_cache.cpp exfat.cpp pfs.cpp ufs2.cpp ffpfsc.cpp png.cpp nas.cpp) \
  $(APP_WORK)/src/core/save_file.cpp $(APP_WORK)/src/third_party/stb_image/stb_image.c
FFPFSC_FIXTURES := build/host/ffpfsc-fixtures

# The Connection screen's lock while a copy needs its settings.
HOST_LOCK := build/host/test_connection_lock
LOCK_SRCS := tests/test_connection_lock.cpp $(LIBSMB2_HOST) \
  $(addprefix $(APP_WORK)/src/concepts/,connection_page.cpp connection.cpp nas_probe.cpp \
    http.cpp folder_picker.cpp) \
  $(KIT_DRAWING) $(KIT_COMPONENTS)

# The queue panel, driven by pad input over a real queue.
HOST_QUEUE := build/host/test_queue_panel
QUEUE_SRCS := tests/test_queue_panel.cpp $(LIBSMB2_HOST) \
  $(addprefix $(APP_WORK)/src/concepts/,queue_panel.cpp transfer.cpp copy_helper.cpp delete_helper.cpp \
    file_calls.cpp smp.cpp json_scan.cpp http.cpp connection.cpp library_cache.cpp notify.cpp \
    storage.cpp app_settings.cpp nas.cpp nas_probe.cpp) \
  $(KIT_DRAWING) $(KIT_COMPONENTS)

# The delete helper (tools/helper/helper.c) and its client (delete_helper.cpp),
# driven through a stand-in ELF loader. HOST_HELPER is the helper built for this
# computer; the client sends it to the fake loader, which runs it.
HOST_HELPER := build/host/helper
# The copy helper (tools/copier/copier.c), built for this computer the same way;
# test_transfer drives it through the stand-in loader.
HOST_COPIER := build/host/copier
HOST_COPIER_SMB := $(call host_objs,$(addprefix $(APP_WORK)/$(LIBSMB2_DIR)/,$(LIBSMB2_OURS)) \
  $(addprefix $(APP_WORK)/$(LIBSMB2_DIR)/lib/,$(LIBSMB2_SRCS)))
$(HOST_COPIER): tools/copier/copier.c $(HOST_COPIER_SMB)
	@mkdir -p $(dir $@)
	clang-18 -std=c11 -Wall -Wextra -Werror -O1 -D_DEFAULT_SOURCE -DCOPIER_HOST_TEST -pthread \
	  -I$(APP_WORK)/$(LIBSMB2_DIR)/include -o $@ $^
$(HOST_HELPER): tools/helper/helper.c
	@mkdir -p $(@D)
	clang-18 -std=c11 -Wall -Wextra -Werror -O1 -D_DEFAULT_SOURCE -DHELPER_HOST_TEST -o $@ $<
# The SMB layer (smb.cpp) and the connection's SMB settings, without a NAS:
# paths, user names, saving, and the failures it explains.
HOST_SMB := build/host/test_smb
SMB_SRCS := tests/test_smb.cpp $(LIBSMB2_HOST) $(APP_WORK)/src/concepts/connection.cpp \
  $(APP_WORK)/src/core/save_file.cpp

# The library and the copy queue over SMB, against a real Samba server
# (tests/smb_server.py), through the copy helper as well as in-app.
HOST_SMB_NAS := build/host/test_smb_nas
SMB_NAS_SRCS := tests/test_smb_nas.cpp $(LIBSMB2_HOST) \
  $(addprefix $(APP_WORK)/src/concepts/,nas.cpp nas_library.cpp library_cache.cpp exfat.cpp pfs.cpp \
    ufs2.cpp ffpfsc.cpp png.cpp nas_probe.cpp http.cpp connection.cpp transfer.cpp copy_helper.cpp \
    delete_helper.cpp file_calls.cpp smp.cpp json_scan.cpp notify.cpp storage.cpp) \
  $(APP_WORK)/src/core/save_file.cpp $(APP_WORK)/src/third_party/stb_image/stb_image.c

HOST_DELETE := build/host/test_delete
DELETE_SRCS := tests/test_delete.cpp \
  $(APP_WORK)/src/concepts/delete_helper.cpp $(APP_WORK)/src/core/save_file.cpp

$(HOST_TRANSFER): $(call host_objs,$(TRANSFER_SRCS))
$(HOST_PICKER): $(call host_objs,$(PICKER_SRCS))
$(HOST_COUNT): $(call host_objs,$(COUNT_SRCS))
$(HOST_FFPFSC): $(call host_objs,$(FFPFSC_SRCS))
$(HOST_LOCK): $(call host_objs,$(LOCK_SRCS))
$(HOST_QUEUE): $(call host_objs,$(QUEUE_SRCS))
$(HOST_DELETE): $(call host_objs,$(DELETE_SRCS))
$(HOST_SMB): $(call host_objs,$(SMB_SRCS))
$(HOST_SMB_NAS): $(call host_objs,$(SMB_NAS_SRCS))
$(HOST_TRANSFER) $(HOST_PICKER) $(HOST_COUNT) $(HOST_FFPFSC) $(HOST_LOCK) $(HOST_QUEUE) $(HOST_DELETE) $(HOST_SMB) \
  $(HOST_SMB_NAS):
	@mkdir -p $(@D)
	clang++-18 -o $@ $^ -lpthread

-include $(shell find $(HOST_OBJ) -name '*.d' 2>/dev/null)

# Each suite is its own target, so `make -j` runs them at the same time.
# The transfer suite, the longest, runs as three parts at once (its own ports
# and work folder each): run-transfer runs all three.
TRANSFER_PARTS := run-transfer-1 run-transfer-2 run-transfer-3
HOST_RUNS := run-installer run-release $(TRANSFER_PARTS) run-picker run-count run-ffpfsc run-lock \
  run-queue run-delete run-smb run-smb-nas
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
run-ffpfsc: $(HOST_FFPFSC)
	@rm -rf $(FFPFSC_FIXTURES) && mkdir -p $(FFPFSC_FIXTURES)
	@cp tests/fixtures/ffpfsc-sce_sys.txt $(FFPFSC_FIXTURES)/
	@for f in tests/fixtures/*.ffpfsc.gz; do gzip -dc $$f > $(FFPFSC_FIXTURES)/$$(basename $$f .gz); done
	$(HOST_FFPFSC) build/host/ffpfsc-test $(FFPFSC_FIXTURES)
run-lock: $(HOST_LOCK)
	cd build/host && ./test_connection_lock
run-queue: $(HOST_QUEUE)
	$(HOST_QUEUE) build/host/queue-test
run-delete: $(HOST_DELETE) $(HOST_HELPER)
	$(HOST_DELETE) build/host/delete-test $(HOST_HELPER)
run-smb: $(HOST_SMB)
	$(HOST_SMB)
run-smb-nas: $(HOST_SMB_NAS) $(HOST_COPIER)
	$(HOST_SMB_NAS) build/host/smb-nas-test $(HOST_COPIER)

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
