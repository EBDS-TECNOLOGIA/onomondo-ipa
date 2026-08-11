#!/bin/bash
#
# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & Iapyx Informatica Ltda. All rights reserved.
#
# SPDX-License-Identifier: AGPL-3.0-only
#
# Cross-build the native dependencies (OpenSSL + libcurl, static) that
# libipacore.so links against, for one Android ABI, using the NDK.
# See ANDROID_PORT_PLAN.md, Phase 0.
#
# This mirrors the Configure/configure flags used by
# https://github.com/ibaoger/libcurl-android but drives the upstream *release*
# tarballs directly, so it needs only `perl` + `make` on the host (no
# autoconf/automake/libtool -- release tarballs ship a pre-generated
# configure/Configure). zlib is intentionally omitted: libcurl is built
# --without-zlib because the IPA core does not need HTTP compression.
#
# Usage:
#   NDK_ROOT=/path/to/android-ndk-r27d \
#     scripts/build-android-deps.sh <abi> [install-prefix]
#
#   <abi>            arm64-v8a | armeabi-v7a | x86_64 | x86   (default arm64-v8a)
#   install-prefix   where headers+libs land (default: ./build-android-deps/<abi>)
#
# Env overrides: API (default 26), OPENSSL_VER (default 3.5.7),
#                CURL_VER (default 8.21.0), JOBS (default nproc).
#
# Feed the resulting prefix to the project build via:
#   cmake -S . -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=<abi> -DANDROID_PLATFORM=android-<API> \
#     -DCMAKE_FIND_ROOT_PATH=<install-prefix> -DOPENSSL_ROOT_DIR=<install-prefix>

set -euo pipefail

ABI="${1:-arm64-v8a}"
API="${API:-26}"
OPENSSL_VER="${OPENSSL_VER:-3.5.7}"
CURL_VER="${CURL_VER:-8.21.0}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 1)}"

if [ -z "${NDK_ROOT:-}" ]; then
	echo "error: set NDK_ROOT to your Android NDK (e.g. .../android-ndk-r27d)" >&2
	exit 1
fi

HOST_TAG="linux-x86_64"
[ "$(uname)" = "Darwin" ] && HOST_TAG="darwin-x86_64"
BIN="$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin"
if [ ! -x "$BIN/llvm-ar" ]; then
	echo "error: NDK toolchain not found at $BIN" >&2
	exit 1
fi

# Per-ABI: OpenSSL Configure target | clang triple (CC prefix) | config --host | extra CFLAGS
case "$ABI" in
arm64-v8a)    SSL_TARGET=android-arm64;  CC_TRIPLE=aarch64-linux-android;     HOST=aarch64-linux-android;   ABI_CFLAGS="" ;;
armeabi-v7a)  SSL_TARGET=android-arm;    CC_TRIPLE=armv7a-linux-androideabi;  HOST=arm-linux-androideabi;   ABI_CFLAGS="-march=armv7-a -mfloat-abi=softfp -mfpu=neon" ;;
x86_64)       SSL_TARGET=android-x86_64; CC_TRIPLE=x86_64-linux-android;      HOST=x86_64-linux-android;    ABI_CFLAGS="" ;;
x86)          SSL_TARGET=android-x86;    CC_TRIPLE=i686-linux-android;        HOST=i686-linux-android;      ABI_CFLAGS="" ;;
*) echo "error: unknown ABI '$ABI'" >&2; exit 1 ;;
esac

PREFIX="${2:-$PWD/build-android-deps/$ABI}"
WORK="$(dirname "$PREFIX")/.src"
mkdir -p "$PREFIX" "$WORK"

fetch() { # url dest
	[ -f "$2" ] && return 0
	echo ">> downloading $(basename "$2")"
	wget -q -O "$2" "$1" || { echo "download failed: $1" >&2; exit 1; }
}

export ANDROID_NDK_ROOT="$NDK_ROOT"
export PATH="$BIN:$PATH"

echo "==================== OpenSSL $OPENSSL_VER ($ABI) ===================="
fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz" \
	"$WORK/openssl-$OPENSSL_VER.tar.gz"
rm -rf "$WORK/openssl-$ABI"; mkdir -p "$WORK/openssl-$ABI"
tar -xzf "$WORK/openssl-$OPENSSL_VER.tar.gz" -C "$WORK/openssl-$ABI" --strip-components=1
( cd "$WORK/openssl-$ABI"
  ./Configure "$SSL_TARGET" no-shared no-tests no-docs no-apps \
      -D__ANDROID_API__="$API" --prefix="$PREFIX" --openssldir="$PREFIX/ssl"
  make -j"$JOBS" build_libs
  make install_dev )

echo "==================== curl $CURL_VER ($ABI) ===================="
fetch "https://github.com/curl/curl/releases/download/curl-${CURL_VER//./_}/curl-$CURL_VER.tar.gz" \
	"$WORK/curl-$CURL_VER.tar.gz"
rm -rf "$WORK/curl-$ABI"; mkdir -p "$WORK/curl-$ABI"
tar -xzf "$WORK/curl-$CURL_VER.tar.gz" -C "$WORK/curl-$ABI" --strip-components=1
( cd "$WORK/curl-$ABI"
  export CC="$BIN/${CC_TRIPLE}${API}-clang"
  export AR="$BIN/llvm-ar" RANLIB="$BIN/llvm-ranlib"
  export CFLAGS="$ABI_CFLAGS -fPIC"
  export CPPFLAGS="-I$PREFIX/include"
  export LDFLAGS="-L$PREFIX/lib -L$PREFIX/lib64"
  ./configure --host="$HOST" --prefix="$PREFIX" \
      --with-openssl="$PREFIX" \
      --without-zlib --without-libpsl --without-brotli --without-zstd \
      --without-libidn2 --without-nghttp2 --without-ngtcp2 \
      --enable-static --disable-shared \
      --disable-manual --disable-ldap --disable-ldaps \
      --enable-ipv6 --enable-threaded-resolver
  make -j"$JOBS" -C lib
  make -C lib install
  make -C include install )

echo "==================== done ($ABI) ===================="
echo "prefix: $PREFIX"
ls -la "$PREFIX"/lib*/libssl.a "$PREFIX"/lib*/libcrypto.a "$PREFIX"/lib/libcurl.a 2>/dev/null || true
