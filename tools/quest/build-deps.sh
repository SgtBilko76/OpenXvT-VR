#!/usr/bin/env bash
# Build the Quest/PICO (Android arm64) dependencies for OpenXvT VR:
#   - host tools: aeron_fsr3_shadergen (must run on the build PC)
#   - SDL3 (main branch, for SDL_gpu OpenXR support), shared
#   - zstd, static
#   - FFmpeg, static, the same codecs as the desktop packages (Smacker movies,
#     WAV, Ogg Vorbis CD music). --disable-asm: the aarch64 NEON FFT code
#     uses non-PIC relocations and cannot be linked into libmain.so.
#   - mbedTLS + libcurl, static (DirectPlay lobby/server browser over HTTPS;
#     certificates come from Android's /system/etc/security/cacerts).
# Everything lands under build/quest/ (git-ignored). Re-runnable: finished
# steps are skipped unless FORCE=1. Source clones are borrowed from
# ~/Projekte/OpenTIE/OpenXWA-VR build/quest/src when present.
#
# Requires the host shadercross from ~/tools/xwa-tools (see OpenXWA docs).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/quest"
SRC="$OUT/src"
PREFIX="$OUT/deps"
HOST="$OUT/host-tools"
BORROW="$HOME/Projekte/OpenTIE/build/quest/src"

NDK="${ANDROID_NDK_ROOT:-$HOME/Android/Sdk/ndk/27.2.12479018}"
API=29
ABI=arm64-v8a
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
JOBS="$(nproc)"

SDL_COMMIT=ce3af25d4
ZSTD_VERSION=1.5.7
FFMPEG_VERSION=8.0.1
MBEDTLS_VERSION=3.6.4
CURL_VERSION=8_22_0

XWA_TOOLS="$HOME/tools/xwa-tools"
export LD_LIBRARY_PATH="$XWA_TOOLS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$HOME/.local/bin:$PATH"

ANDROID_CMAKE=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI="$ABI"
    -DANDROID_PLATFORM="android-$API"
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_PREFIX_PATH="$PREFIX"
    -DCMAKE_FIND_ROOT_PATH="$PREFIX"
)

mkdir -p "$SRC" "$PREFIX" "$HOST"

done_marker() { [[ "${FORCE:-0}" != 1 && -f "$OUT/.done-$1" ]]; }
mark_done() { touch "$OUT/.done-$1"; }
borrow() { # borrow <name>: copy a source clone from OpenTIE if we lack it
    if [[ ! -d "$SRC/$1" && -d "$BORROW/$1" ]]; then
        echo "   (borrowing $1 sources from OpenTIE)"
        cp -a "$BORROW/$1" "$SRC/$1"
    fi
}

# ---------------------------------------------------------------- host tools
if ! done_marker host-shadergen; then
    echo "== host: aeron_fsr3_shadergen"
    cmake -S "$ROOT/aeron" -B "$OUT/build-host-tools" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DAERON_HOST_TOOLS_ONLY=ON \
        -DCMAKE_INSTALL_PREFIX="$HOST"
    cmake --build "$OUT/build-host-tools" --parallel "$JOBS"
    cmake --install "$OUT/build-host-tools"
    mark_done host-shadergen
fi

# ---------------------------------------------------------------------- SDL3
if ! done_marker sdl3; then
    echo "== SDL3 @ $SDL_COMMIT"
    borrow SDL
    [[ -d "$SRC/SDL/.git" ]] || git clone https://github.com/libsdl-org/SDL.git "$SRC/SDL"
    git -C "$SRC/SDL" cat-file -e "$SDL_COMMIT^{commit}" 2>/dev/null || git -C "$SRC/SDL" fetch --quiet origin
    git -C "$SRC/SDL" checkout --quiet "$SDL_COMMIT"
    cmake -S "$SRC/SDL" -B "$OUT/build-sdl3" "${ANDROID_CMAKE[@]}" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_INSTALL_DOCS=OFF
    cmake --build "$OUT/build-sdl3" --parallel "$JOBS"
    cmake --install "$OUT/build-sdl3"
    mark_done sdl3
fi

# ---------------------------------------------------------------------- zstd
if ! done_marker zstd; then
    echo "== zstd $ZSTD_VERSION"
    borrow zstd
    [[ -d "$SRC/zstd" ]] || git clone --quiet --branch "v$ZSTD_VERSION" --depth 1 \
        https://github.com/facebook/zstd.git "$SRC/zstd"
    cmake -S "$SRC/zstd/build/cmake" -B "$OUT/build-zstd" "${ANDROID_CMAKE[@]}" \
        -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
        -DZSTD_BUILD_TESTS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    cmake --build "$OUT/build-zstd" --parallel "$JOBS"
    cmake --install "$OUT/build-zstd"
    mark_done zstd
fi

# -------------------------------------------------------------------- FFmpeg
if ! done_marker ffmpeg; then
    echo "== FFmpeg $FFMPEG_VERSION"
    borrow FFmpeg
    [[ -d "$SRC/FFmpeg" ]] || git clone --quiet --branch "n$FFMPEG_VERSION" --depth 1 \
        https://github.com/FFmpeg/FFmpeg.git "$SRC/FFmpeg"
    rm -rf "$OUT/build-ffmpeg" && mkdir -p "$OUT/build-ffmpeg"
    (
        cd "$OUT/build-ffmpeg"
        "$SRC/FFmpeg/configure" \
            --prefix="$PREFIX" \
            --target-os=android \
            --arch=aarch64 \
            --cpu=armv8-a \
            --enable-cross-compile \
            --sysroot="$TC/sysroot" \
            --cc="$TC/bin/aarch64-linux-android$API-clang" \
            --cxx="$TC/bin/aarch64-linux-android$API-clang++" \
            --ar="$TC/bin/llvm-ar" \
            --nm="$TC/bin/llvm-nm" \
            --ranlib="$TC/bin/llvm-ranlib" \
            --strip="$TC/bin/llvm-strip" \
            --pkg-config=false \
            --disable-autodetect \
            --disable-avdevice \
            --disable-avfilter \
            --disable-debug \
            --disable-doc \
            --disable-everything \
            --disable-network \
            --disable-programs \
            --disable-shared \
            --enable-static \
            --enable-pic \
            --disable-asm \
            --enable-pthreads \
            --enable-swresample \
            --enable-swscale \
            --enable-decoder=smacker,smackaud,pcm_u8,pcm_s16le,vorbis,binkaudio_rdft,binkaudio_dct \
            --enable-parser=vorbis \
            --enable-demuxer=smacker,ogg,wav
        make -j"$JOBS"
        make install
    )
    mark_done ffmpeg
fi

# ------------------------------------------------------------------- mbedTLS
if ! done_marker mbedtls; then
    echo "== mbedTLS $MBEDTLS_VERSION"
    [[ -d "$SRC/mbedtls" ]] || git clone --quiet --branch "mbedtls-$MBEDTLS_VERSION" --depth 1 \
        --recurse-submodules --shallow-submodules \
        https://github.com/Mbed-TLS/mbedtls.git "$SRC/mbedtls"
    cmake -S "$SRC/mbedtls" -B "$OUT/build-mbedtls" "${ANDROID_CMAKE[@]}" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    cmake --build "$OUT/build-mbedtls" --parallel "$JOBS"
    cmake --install "$OUT/build-mbedtls"
    mark_done mbedtls
fi

# ---------------------------------------------------------------------- curl
if ! done_marker curl; then
    echo "== curl $CURL_VERSION"
    [[ -d "$SRC/curl" ]] || git clone --quiet --branch "curl-$CURL_VERSION" --depth 1 \
        https://github.com/curl/curl.git "$SRC/curl"
    cmake -S "$SRC/curl" -B "$OUT/build-curl" "${ANDROID_CMAKE[@]}" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
        -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DMbedTLS_ROOT="$PREFIX" \
        -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=/system/etc/security/cacerts \
        -DENABLE_THREADED_RESOLVER=ON -DHTTP_ONLY=ON \
        -DCURL_USE_LIBPSL=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
        -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF
    cmake --build "$OUT/build-curl" --parallel "$JOBS"
    cmake --install "$OUT/build-curl"
    mark_done curl
fi

echo "== deps ready in $PREFIX"
ls "$PREFIX/lib"
