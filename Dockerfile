# NASBridge PS5 toolchain image.
#
# Everything the build needs lives here, so `make build` behaves the same on
# every machine. It builds natively for the host's CPU (arm64 on Apple
# Silicon, amd64 elsewhere); the PS5 output is meant to be identical either way.
#
# To upgrade a tool, change the pinned value below and rebuild with
# `make image`. Never fetch anything at build time outside this file.

# Pinned to an exact image (multi-arch: arm64 and amd64). To upgrade, see
# README, "Updating the base image".
FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

ARG DEBIAN_FRONTEND=noninteractive

# Ubuntu 24.04 ships LLVM 18, the version both PS5 toolchains are tested with.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bash \
      ca-certificates \
      clang-18 \
      cmake \
      curl \
      file \
      git \
      libc6-dev-amd64-cross \
      libegl-dev \
      libegl-mesa0 \
      libgl-dev \
      libgl1-mesa-dri \
      lld-18 \
      llvm-18 \
      make \
      ninja-build \
      python3 \
      python3-pyelftools \
      socat \
      unzip \
      wget \
      xz-utils \
 && rm -rf /var/lib/apt/lists/*

# PS5 payload SDK (ps5-payload-dev/sdk), built from source at a pinned
# release. Used for the payloads: the installer and the copy and delete
# helpers. The commit hash is checked so a moved tag can't change the build.
ARG PS5_SDK_REPO=https://github.com/ps5-payload-dev/sdk.git
ARG PS5_SDK_TAG=v0.44
ARG PS5_SDK_COMMIT=f5d40f02770e9b0001dd9e18056eca70b3da8b40
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

RUN git clone --depth 1 --branch "$PS5_SDK_TAG" "$PS5_SDK_REPO" /tmp/ps5-sdk \
 && test "$(git -C /tmp/ps5-sdk rev-parse HEAD)" = "$PS5_SDK_COMMIT" \
      || { echo "PS5 SDK tag $PS5_SDK_TAG is not commit $PS5_SDK_COMMIT" >&2; exit 1; } \
 && make -C /tmp/ps5-sdk -s DESTDIR="$PS5_PAYLOAD_SDK" install \
 && rm -rf /tmp/ps5-sdk

# compiler-rt builtins for x86-64, built from LLVM's source to match clang
# 18.1.3. The UI kit links them into the PS5 app. Ubuntu only packages them for
# the host's own CPU, so on Apple Silicon there's no x86-64 copy to install;
# building from pinned, checksummed source gives every host the same file.
# The x86-64 C headers come from Ubuntu's cross package on every host.
ARG LLVM_VERSION=18.1.3
ARG LLVM_RELEASES=https://github.com/llvm/llvm-project/releases/download
RUN mkdir -p /tmp/crt/src && cd /tmp/crt \
 && curl -fsSL -o compiler-rt.tar.xz "$LLVM_RELEASES/llvmorg-$LLVM_VERSION/compiler-rt-$LLVM_VERSION.src.tar.xz" \
 && curl -fsSL -o cmake.tar.xz "$LLVM_RELEASES/llvmorg-$LLVM_VERSION/cmake-$LLVM_VERSION.src.tar.xz" \
 && curl -fsSL -o llvm.tar.xz "$LLVM_RELEASES/llvmorg-$LLVM_VERSION/llvm-$LLVM_VERSION.src.tar.xz" \
 && printf '%s\n' \
      "9a7df9300413696b0c4f7ff1e2729cb82aca375f35c05d698c44f26a4edf1c27  compiler-rt.tar.xz" \
      "acfecb615d41c5b1a0a31e15324994ca06f7a3f37d8958d719b20de0d217b71b  cmake.tar.xz" \
      "fa6db8951f5ef576ac6bad43d5e1ed83962754538c998fbfa0397cd4521abc00  llvm.tar.xz" \
    | sha256sum --check --strict \
 && mkdir -p src/compiler-rt src/cmake src/llvm \
 && tar -xf compiler-rt.tar.xz -C src/compiler-rt --strip-components=1 \
 && tar -xf cmake.tar.xz -C src/cmake --strip-components=1 \
 && tar -xf llvm.tar.xz -C src/llvm --strip-components=1 "llvm-$LLVM_VERSION.src/cmake" \
 && cmake -S src/compiler-rt -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
      -DCMAKE_AR=/usr/bin/llvm-ar-18 -DCMAKE_RANLIB=/usr/bin/llvm-ranlib-18 \
      -DCMAKE_C_COMPILER_TARGET=x86_64-linux-gnu -DCMAKE_ASM_COMPILER_TARGET=x86_64-linux-gnu \
      -DCMAKE_C_FLAGS="-isystem /usr/x86_64-linux-gnu/include" \
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
      -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BUILD_BUILTINS=ON \
      -DCOMPILER_RT_BUILD_CRT=OFF -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
      -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
      -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF \
      -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
      -DCOMPILER_RT_INCLUDE_TESTS=OFF \
 && ninja -C build builtins \
 && dest="$(clang-18 --print-resource-dir)/lib/linux" \
 && mkdir -p "$dest" \
 && cp build/lib/linux/libclang_rt.builtins-x86_64.a "$dest/" \
 && cd / && rm -rf /tmp/crt

# UI kit (BlackBearReloaded's ps5-homebrew-ui, GPL-3.0+) at a pinned commit.
# The original repository is no longer public, so this is J7mbo's copy of it:
# its full history up to bbe19054, which NASBridge built with until then, plus
# one commit that fetches the ps5-opengl SDK from J7mbo/ps5-opengl (the same
# SDK, repacked; the kit's manifest check proves it).
# OpenGL renderer, components and the Acrylic theme, plus the native app
# pipeline it inherits from ps5-native-app-boilerplate (compiler flags, the
# eboot.bin converter and its clean-room libc.prx runtime). The app is built
# from a copy of it with our screens in place of its demo designs. Its pinned dependencies
# (payload SDK v0.42, zlib, the ps5-opengl SDK) are fetched and checksum-verified
# here, and its own demo app is built once so a broken toolchain fails now,
# not on someone's first `make build`.
# zlib comes from its GitHub release (the same archive the kit pins; its
# script checks the SHA-256) rather than zlib.net, which is often unreachable.
ARG ZLIB_URL=https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
ARG UI_KIT_REPO=https://github.com/J7mbo/ps5-homebrew-ui.git
ARG UI_KIT_COMMIT=0f5742db7d5b767f58a1eb62544f5e43b7c411d2
ENV UI_KIT=/opt/ps5-homebrew-ui

RUN git clone --quiet "$UI_KIT_REPO" "$UI_KIT" \
 && git -C "$UI_KIT" checkout --quiet "$UI_KIT_COMMIT" \
 && cd "$UI_KIT" \
 && mkdir -p .deps/native/zlib \
 && wget -q "$ZLIB_URL" -O .deps/native/zlib/zlib-1.3.2.tar.gz \
 && make app USE_CCACHE=0 HOST_CC=clang-18 HOST_CXX=clang++-18 \
 && test -f dist/PPSA99050/eboot.bin \
 && echo "$UI_KIT_COMMIT" > .nasbridge-commit \
 && rm -rf .git build dist \
 && chmod -R a+rX "$UI_KIT"

# stb_image (Sean Barrett's single-file image loader; public domain or MIT,
# GPL-compatible) at a pinned commit. NASBridge decodes the games' PNG covers
# with it; the build copies the header into the app.
ARG STB_REPO=https://github.com/nothings/stb.git
ARG STB_COMMIT=2c980bb59875b0d32144a71867fbdebb2f77cd20
ENV STB=/opt/stb

RUN git init -q "$STB" && cd "$STB" \
 && git fetch -q --depth 1 "$STB_REPO" "$STB_COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$STB_COMMIT" \
      || { echo "stb commit $STB_COMMIT not found" >&2; exit 1; } \
 && test -f stb_image.h && test -f LICENSE \
 && echo "$STB_COMMIT" > .nasbridge-commit \
 && rm -rf .git \
 && chmod -R a+rX "$STB"

# Record exact tool versions inside the image (shown by `make toolchain-info`).
RUN { echo "ps5-payload-sdk $PS5_SDK_TAG ($PS5_SDK_COMMIT)"; \
      echo "ps5-homebrew-ui $UI_KIT_COMMIT"; \
      echo "stb $STB_COMMIT"; \
      echo "compiler-rt builtins $LLVM_VERSION (x86-64, from source)"; \
      clang-18 --version | head -1; \
      ld.lld-18 --version; \
      . /etc/os-release && echo "$PRETTY_NAME"; } > /etc/nasbridge-toolchain

WORKDIR /src
