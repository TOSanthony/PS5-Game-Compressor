#!/usr/bin/env bash
# Cross-builds OpenSSL and libcurl (static) for the PS5 into the payload SDK's
# sysroot, as the Orbit Store project does (same pinned versions and hashes).
# Used by the self-update module (src/gc_update.c). Needs PS5_PAYLOAD_SDK.
set -euo pipefail
: "${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK is required}"
work=${GC_DEPS_WORK:-/tmp/gc-tls-deps}
mkdir -p "$work"
cd "$work"
fetch() { # url file sha256
  [[ -f $2 ]] || curl --silent --show-error --fail --location --retry 3 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | sha256sum -c -
  tar xf "$2"
}
fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz openssl.tar.gz c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec
fetch https://github.com/curl/curl/releases/download/curl-8_18_0/curl-8.18.0.tar.xz curl.tar.xz 40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946
# shellcheck disable=SC1091
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
export CPPFLAGS="-I$PS5_SYSROOT/user/homebrew/include"
export LDFLAGS="-L$PS5_SYSROOT/user/homebrew/lib"
jobs=${JOBS:-$(nproc)}
if [[ ! -f $PS5_SYSROOT/user/homebrew/lib/libssl.a ]]; then
  (cd openssl-3.5.2 &&
    ./Configure BSD-x86_64 no-tests no-apps no-shared --prefix="$PREFIX" &&
    make -j"$jobs" build_sw && make install_sw DESTDIR="$PS5_SYSROOT")
fi
if [[ ! -f $PS5_SYSROOT/user/homebrew/lib/libcurl.a ]]; then
  (cd curl-8.18.0 &&
    ./configure --prefix="$PREFIX" --host=x86_64-pc-freebsd --enable-static --disable-shared \
      --with-openssl="$PS5_SYSROOT/user/homebrew" --without-zlib --without-brotli --without-zstd \
      --without-libpsl --without-libidn2 --without-libssh2 --disable-ldap --disable-ldaps \
      --disable-docs --disable-manual --disable-ftp --disable-file --disable-dict --disable-gopher \
      --disable-imap --disable-mqtt --disable-pop3 --disable-rtsp --disable-smb --disable-smtp \
      --disable-telnet --disable-tftp --disable-websockets --disable-ipv6 &&
    make -j"$jobs" && make install DESTDIR="$PS5_SYSROOT")
fi
ls -la "$PS5_SYSROOT/user/homebrew/lib/libssl.a" "$PS5_SYSROOT/user/homebrew/lib/libcrypto.a" "$PS5_SYSROOT/user/homebrew/lib/libcurl.a"
