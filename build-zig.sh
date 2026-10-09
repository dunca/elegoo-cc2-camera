#!/usr/bin/env bash
# Cross-build ai_camera for the Elegoo Centauri Carbon 2 with zig (works on macOS and Linux, no musl.cc toolchain).
#
# Usage: ./build-zig.sh            -> build/zig-<cpu>/ai_camera (+ stagebench)
# CC2_CPU selects the zig CPU model (default: cortex_a7 = the CC2's Allwinner R528, ARMv7 + NEON + VFPv4).
#   CC2_CPU=generic+v7a+vfp3d16-neon reproduces the upstream release target (generic ARMv7, no NEON in C code).
# Requirements: zig (brew install zig), git, make.
set -euo pipefail

CPU="${CC2_CPU:-cortex_a7}"
TAG="$(printf '%s' "$CPU" | tr -c 'a-zA-Z0-9_\n' '_')"
X264_COMMIT=0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee

# x264's configure breaks on paths with spaces, so work through a space-free symlink.
REAL_ROOT="$(cd "$(dirname "$0")" && pwd)"
LINK_DIR="$(mktemp -d)"
trap 'rm -f "$LINK_DIR/src"; rmdir "$LINK_DIR"' EXIT
ln -s "$REAL_ROOT" "$LINK_DIR/src"
ROOT="$LINK_DIR/src"
cd "$ROOT"

TC="$ROOT/build/zigtc-$TAG"
SYSROOT="$ROOT/build/sysroot-$TAG"
OUT="$ROOT/build/zig-$TAG"
mkdir -p "$TC" "$OUT/obj"

# Toolchain wrappers with the GNU names x264's configure expects.
printf '#!/bin/sh\nexec zig cc -target arm-linux-musleabihf -mcpu=%s "$@"\n' "$CPU" > "$TC/arm-gcc"
printf '#!/bin/sh\nexec zig ar "$@"\n' > "$TC/arm-ar"
printf '#!/bin/sh\nexec zig ranlib "$@"\n' > "$TC/arm-ranlib"
printf '#!/bin/sh\nexit 0\n' > "$TC/arm-strip"
printf '#!/bin/sh\nexec strings -a "$@"\n' > "$TC/arm-strings"
chmod +x "$TC"/arm-*
CC="$TC/arm-gcc"

# libx264, pinned, built out of tree once per CPU model.
if [[ ! -d build/x264-src/.git ]]; then
  git clone -q https://code.videolan.org/videolan/x264.git build/x264-src
fi
git -C build/x264-src checkout -q "$X264_COMMIT"
if [[ ! -f "$SYSROOT/lib/libx264.a" ]]; then
  mkdir -p "build/x264-$TAG"
  (cd "build/x264-$TAG" && "$ROOT/build/x264-src/configure" \
      --host=arm-linux-gnueabihf --cross-prefix="$TC/arm-" --prefix="$SYSROOT" \
      --enable-static --disable-cli --disable-opencl --disable-avs --disable-ffms --disable-gpac \
      --disable-lsmash --disable-lavf --disable-swscale --disable-interlaced --enable-pic \
      --extra-cflags="-O3 -mcpu=$CPU" > configure.log \
    && make -j"$(getconf _NPROCESSORS_ONLN)" > make.log 2>&1 && make install > /dev/null)
fi

VERSION="$(sed -n 's/^VERSION ?= //p' Makefile)"
GIT_VER="$(git describe --tags --always --dirty 2>/dev/null || echo "v$VERSION")"
CFLAGS=(-O3 -Isrc -D_GNU_SOURCE "-DAPP_VERSION=\"$VERSION\"" "-DGIT_VERSION=\"$GIT_VER\""
        -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 "-I$SYSROOT/include" -w)
LDFLAGS=("-L$SYSROOT/lib" -static -s -pthread -lx264 -lm)

OBJS=()
for src in main frame_ring v4l2_camera http_server uds_ipc timelapse_encoder db_sync sqlite3; do
  obj="$OUT/obj/$src.o"
  if [[ ! -f "$obj" || "src/$src.c" -nt "$obj" || "$0" -nt "$obj" || -n "$(find src -name '*.h' -newer "$obj")" ]]; then
    "$CC" "${CFLAGS[@]}" -c "src/$src.c" -o "$obj"
  fi
  OBJS+=("$obj")
done
"$CC" "${OBJS[@]}" "${LDFLAGS[@]}" -o "$OUT/ai_camera"
"$CC" "${CFLAGS[@]}" tools/stagebench.c "${LDFLAGS[@]}" -o "$OUT/stagebench"

echo "built $OUT/ai_camera ($GIT_VER, cpu $CPU)" | sed "s|$ROOT|.|"
