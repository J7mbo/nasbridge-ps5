# NASBridge for PS5 — build entry point.
#
# Every build step runs inside the toolchain container, so the only things
# you need installed are Docker (Docker Desktop or OrbStack) and make.
#
#   make build                   build everything into out/: the home-screen
#                                app folder and the installer payload
#   make install PS5_HOST=<ip>   install the app on the PS5's home screen
#   make release                 build out/NASBridge-<version>.elf: one payload
#                                that installs or updates the app on its own
#   make install-selftest PS5_HOST=<ip>
#                                check the installer by copying a test folder
#   make snapshots               render the app's screens to out/snapshots
#   make test                    run the installer tests
#   make kit-check               build the UI kit's demo app and preview pictures
#   make install-kit-lab PS5_HOST=<ip>
#                                install the UI kit's demo app (Homebrew UI Lab)
#   make shell                   open a shell inside the toolchain container
#   make toolchain-info          print the exact tool versions in use
#   make image                   rebuild the toolchain image
#   make clean                   remove build output

DOCKER   ?= docker
PS5_HOST ?=
PS5_PORT ?= 9021

# The image tag follows the Dockerfile's contents, so editing the Dockerfile
# automatically produces a fresh image instead of reusing a stale one.
DOCKERFILE_HASH := $(shell (sha256sum Dockerfile 2>/dev/null || shasum -a 256 Dockerfile) | cut -c1-12)
IMAGE           := nasbridge-ps5-toolchain:$(DOCKERFILE_HASH)

# Build metadata: taken from git when available, fixed values otherwise, so
# the same source always produces the same payload.
BUILD_ID          := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo local)
SOURCE_DATE_EPOCH := $(shell git log -1 --format=%ct 2>/dev/null || echo 0)

DOCKER_RUN := $(DOCKER) run --rm \
  -v "$(CURDIR)":/src \
  -u "$$(id -u):$$(id -g)" \
  -e BUILD_ID=$(BUILD_ID) \
  -e SOURCE_DATE_EPOCH=$(SOURCE_DATE_EPOCH) \
  $(IMAGE)

.PHONY: release uninstall build snapshots test kit-check install-kit-lab install install-selftest check-host shell toolchain-info image ensure-image clean distclean

build: ensure-image
	$(DOCKER_RUN) make -f ps5.mk all

snapshots: ensure-image
	$(DOCKER_RUN) make -f ps5.mk snapshots

# One ELF to send to the PS5's ELF loader (port 9021): it installs or updates
# NASBridge without a computer, for GitHub releases.
release: ensure-image
	$(DOCKER_RUN) make -f ps5.mk release

test: ensure-image
	$(DOCKER_RUN) sh -c 'make -f ps5.mk -j"$$(nproc)" --output-sync=target test'

kit-check: ensure-image
	$(DOCKER_RUN) make -f ps5.mk kit-check

install-kit-lab: check-host build
	@test -d out/kit-lab || { echo "Run make kit-check first" >&2; exit 1; }
	$(INSTALL_RUN) --root homebrew $(wildcard out/kit-lab/PPSA*)


# Copies a folder to the PS5 through the installer payload (see
# tools/installer/installer.c): sends the installer via the ELF loader, then
# streams the folder to it. Runs in the container, which has python3.
INSTALL_RUN = $(DOCKER) run --rm -v "$(CURDIR)":/src $(IMAGE) \
  python3 tools/install.py --host $(PS5_HOST) --loader-port $(PS5_PORT) \
  --installer out/installer.elf

install-selftest: check-host build
	$(INSTALL_RUN) --root selftest build/ps5/selftest/NASBSELFTEST

# The app folder is out/app/<title ID> (from app/sce_sys/param.json).
install: check-host build
	$(INSTALL_RUN) --root homebrew out/app/$(TITLE_ID)

# Removes NASBridge from the PS5: its folder in /data/homebrew (ShadowMountPlus
# adds every folder there to the home screen, so deleting only the tile brings
# it back), then its home-screen entry. Refused while NASBridge is open.
TITLE_ID = $(shell python3 -c 'import json;print(json.load(open("app/sce_sys/param.json"))["titleId"])')
uninstall: check-host ensure-image
	$(DOCKER_RUN) make -f ps5.mk out/installer.elf
	$(INSTALL_RUN) --uninstall $(TITLE_ID)

check-host:
	@test -n "$(strip $(PS5_HOST))" || { \
	  echo "Set your PS5's IP address, e.g. make install PS5_HOST=192.168.1.50" >&2; exit 1; }

shell: ensure-image
	$(DOCKER) run --rm -it -v "$(CURDIR)":/src $(IMAGE) bash

toolchain-info: ensure-image
	@$(DOCKER) run --rm $(IMAGE) cat /etc/nasbridge-toolchain

# Builds the image only if this exact version doesn't exist yet.
ensure-image:
	@$(DOCKER) image inspect $(IMAGE) >/dev/null 2>&1 || $(MAKE) --no-print-directory image

image:
	$(DOCKER) build -t $(IMAGE) .

clean:
	rm -rf build out

# Also removes every toolchain image this project has built.
distclean: clean
	-$(DOCKER) images -q nasbridge-ps5-toolchain | xargs $(DOCKER) rmi -f
