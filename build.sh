#!/usr/bin/env bash
# Cross-build ai_camera for the Elegoo Centauri Carbon 2 (Allwinner R528: Cortex-A7, NEON, VFPv4, musl).
#
# Usage: ./build.sh                      -> build/zig-cortex_a7/ai_camera (+ stagebench)
#        CC2_TOOLCHAIN=gcc ./build.sh    -> build/gcc-cortex_a7/ai_camera
#
# CC2_TOOLCHAIN=zig (default)  zig cc (clang + bundled musl). Works on macOS and Linux.
#   CC2_CPU      zig CPU model (default cortex_a7). generic+v7a+vfp3d16-neon = upstream release target.
# CC2_TOOLCHAIN=gcc            musl.cc armv7l-linux-musleabihf gcc (the upstream toolchain). x86_64 Linux only,
#                              e.g. in Docker: debian:bookworm-slim + curl git make cmake binutils.
#   CC2_GCC_CPU  gcc CPU flags (default "-mcpu=cortex-a7 -mfpu=neon-vfpv4"; empty = upstream release target).
# Requirements: git, make, cmake, and zig (brew install zig) or curl for the gcc toolchain.
set -euo pipefail

TOOLCHAIN="${CC2_TOOLCHAIN:-zig}"
X264_COMMIT=0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee
LIBJPEG_TURBO_TAG=3.1.4.1
MUSL_CROSS_URL=https://musl.cc/armv7l-linux-musleabihf-cross.tgz

case "$TOOLCHAIN" in
  zig) CPU="${CC2_CPU:-cortex_a7}" ;;
  gcc) CPU="${CC2_GCC_CPU--mcpu=cortex-a7 -mfpu=neon-vfpv4}" ;;
  *) echo "CC2_TOOLCHAIN must be zig or gcc" >&2; exit 1 ;;
esac
CPU_TAG="$(printf '%s' "$CPU" | sed -e 's/-mcpu=//' -e 's/ *-mfpu=[^ ]*//' | tr -c 'a-zA-Z0-9_\n' '_')"
TAG="$TOOLCHAIN-${CPU_TAG:-generic}"

# x264's configure breaks on paths with spaces, so work through a space-free symlink.
REAL_ROOT="$(cd "$(dirname "$0")" && pwd)"
LINK_DIR="$(mktemp -d)"
trap 'rm -f "$LINK_DIR/src"; rmdir "$LINK_DIR"' EXIT
ln -s "$REAL_ROOT" "$LINK_DIR/src"
ROOT="$LINK_DIR/src"
cd "$ROOT"

OUT="$ROOT/build/$TAG"
TC="$OUT/toolchain"
SYSROOT="$OUT/sysroot"
mkdir -p "$TC" "$OUT/obj"

# Toolchain wrappers with the GNU names x264's configure expects.
if [[ "$TOOLCHAIN" == zig ]]; then
  printf '#!/bin/sh\nexec zig cc -target arm-linux-musleabihf -mcpu=%s "$@"\n' "$CPU" > "$TC/arm-gcc"
  printf '#!/bin/sh\nexec zig ar "$@"\n' > "$TC/arm-ar"
  printf '#!/bin/sh\nexec zig ranlib "$@"\n' > "$TC/arm-ranlib"
  X264_CPU_FLAGS="-mcpu=$CPU"
else
  MC="$ROOT/build/musl-cross"
  if [[ ! -x "$MC/bin/armv7l-linux-musleabihf-gcc" ]]; then
    mkdir -p "$MC"
    curl -fsSL "$MUSL_CROSS_URL" | tar -C "$MC" --strip-components=1 -xz
  fi
  printf '#!/bin/sh\nexec %s %s "$@"\n' "$MC/bin/armv7l-linux-musleabihf-gcc" "$CPU" > "$TC/arm-gcc"
  printf '#!/bin/sh\nexec %s "$@"\n' "$MC/bin/armv7l-linux-musleabihf-ar" > "$TC/arm-ar"
  printf '#!/bin/sh\nexec %s "$@"\n' "$MC/bin/armv7l-linux-musleabihf-ranlib" > "$TC/arm-ranlib"
  # With no CPU flag, x264's configure adds -mcpu=cortex-a8 -mfpu=neon itself, as in the upstream build.
  X264_CPU_FLAGS="$CPU"
fi
printf '#!/bin/sh\nexit 0\n' > "$TC/arm-strip"
printf '#!/bin/sh\nexec strings -a "$@"\n' > "$TC/arm-strings"
chmod +x "$TC"/arm-*
CC="$TC/arm-gcc"

# libx264, pinned, built out of tree once per toolchain/CPU.
if [[ ! -d build/x264-src/.git ]]; then
  git clone -q https://code.videolan.org/videolan/x264.git build/x264-src
fi
git -C build/x264-src checkout -q "$X264_COMMIT"
if [[ ! -f "$SYSROOT/lib/libx264.a" ]]; then
  mkdir -p "$OUT/x264"
  (cd "$OUT/x264" && "$ROOT/build/x264-src/configure" \
      --host=arm-linux-gnueabihf --cross-prefix="$TC/arm-" --prefix="$SYSROOT" \
      --enable-static --disable-cli --disable-opencl --disable-avs --disable-ffms --disable-gpac \
      --disable-lsmash --disable-lavf --disable-swscale --disable-interlaced --enable-pic \
      --extra-cflags="-O3 $X264_CPU_FLAGS" > configure.log \
    && make -j"$(getconf _NPROCESSORS_ONLN)" > make.log 2>&1 && make install > /dev/null)
fi

# libjpeg-turbo (TurboJPEG API, NEON SIMD), pinned, static, once per toolchain/CPU.
if [[ ! -d build/libjpeg-turbo-src/.git ]]; then
  git clone -q https://github.com/libjpeg-turbo/libjpeg-turbo.git build/libjpeg-turbo-src
fi
git -C build/libjpeg-turbo-src checkout -q "$LIBJPEG_TURBO_TAG"
if [[ ! -f "$SYSROOT/lib/libturbojpeg.a" ]]; then
  cat > "$TC/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER $TC/arm-gcc)
set(CMAKE_ASM_COMPILER $TC/arm-gcc)
set(CMAKE_AR $TC/arm-ar)
set(CMAKE_RANLIB $TC/arm-ranlib)
EOF
  cmake -S build/libjpeg-turbo-src -B "$OUT/libjpeg-turbo" \
    -DCMAKE_TOOLCHAIN_FILE="$TC/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
    -DWITH_TURBOJPEG=ON -DWITH_JAVA=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_SIMD=ON \
    -DREQUIRE_SIMD=ON > "$OUT/libjpeg-turbo.configure.log"
  cmake --build "$OUT/libjpeg-turbo" -j"$(getconf _NPROCESSORS_ONLN)" > "$OUT/libjpeg-turbo.make.log"
  cmake --install "$OUT/libjpeg-turbo" > /dev/null
fi

VERSION="$(sed -n 's/^VERSION ?= //p' Makefile)"
GIT_VER="$(git describe --tags --always --dirty 2>/dev/null || echo "v$VERSION")"
CFLAGS=(-O3 -Isrc -D_GNU_SOURCE "-DAPP_VERSION=\"$VERSION\"" "-DGIT_VERSION=\"$GIT_VER\""
        -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DHAVE_TURBOJPEG "-I$SYSROOT/include" -w)
LDFLAGS=("-L$SYSROOT/lib" -static -s -pthread -lx264 -lturbojpeg -lm)

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

echo "built $OUT/ai_camera ($GIT_VER, $TOOLCHAIN ${CPU:-generic})" | sed "s|$ROOT|.|"
