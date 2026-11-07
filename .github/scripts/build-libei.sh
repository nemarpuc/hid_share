#!/usr/bin/env bash
# Builds libei (the client library only) for the Linux builds. Ubuntu 22.04, the
# oldest system the archives are built on, has no libei package, and the portal
# backend needs it. The source is the tarball Debian ships, checked against the
# SHA-256 pinned here; the build is installed under $GITHUB_WORKSPACE/libei-prefix
# and the release archives carry libei.so.1 next to the programs.
set -euo pipefail

version=1.5.0
sha256=da1fba92daccd0667bc46c3ee952d4ae8cfc6bdb4c0bb4d34df26528fb240618
url="https://deb.debian.org/debian/pool/main/libe/libei/libei_${version}.orig.tar.bz2"

work="${RUNNER_TEMP:-/tmp}/libei-build"
prefix="${GITHUB_WORKSPACE:?}/libei-prefix"
rm -rf "$work"
mkdir -p "$work"
cd "$work"
curl -fsSL --retry 3 -o libei.tar.bz2 "$url"
echo "${sha256}  libei.tar.bz2" | sha256sum -c -
tar -xjf libei.tar.bz2
cd "libei-${version}"
meson setup build --prefix "$prefix" --libdir lib --buildtype release \
  -Dtests=disabled -Dliboeffis=disabled -Dlibeis=disabled
ninja -C build
ninja -C build install
install -D COPYING "$prefix/share/doc/libei/COPYING"

{
  echo "LIBEI_PREFIX=$prefix"
  echo "PKG_CONFIG_PATH=$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  echo "LD_LIBRARY_PATH=$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
} >> "$GITHUB_ENV"
