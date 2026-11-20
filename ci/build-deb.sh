#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build Debian packages of this tree the way Armbian builds its Mesa (armbian/external build/mesa.yaml
# and extpkg/builder.py): this tree as upstream, Debian sid's packaging, Armbian's packaging patches,
# the meson subprojects Armbian downloads, then dpkg-buildpackage -b. All drivers, so one Mesa serves
# both the CIX vendor stack (zink on libmali) and Panthor (panfrost, PanVK).
#
#   ci/build-deb.sh <work dir> [--install-deps]
#
# --install-deps runs "apt-get build-dep" (as root, e.g. in CI); otherwise the build dependencies must
# already be installed (see dpkg-checkbuilddeps). Packaging inputs are fetched unless given:
#   DEBIAN_DIR=<dir with debian/>          Debian's packaging at tag $DEBIAN_TAG
#   ARMBIAN_PATCHES=<dir with series>      armbian/external build/mesa/patches at $ARMBIAN_COMMIT
# The version is Armbian's (the base of the armbian/resolute branch) plus "+cix<commit date>", so every
# newer commit sorts higher. Output: <work dir>/*.deb, log in <work dir>/build.log.
set -euo pipefail

# Armbian's Mesa that branch armbian/resolute reproduces: update all three when rebasing onto a newer one.
BASE_VERSION=27.0.0~git202610251245.e737139f66-1+armbian1.1
DEBIAN_TAG=debian/26.2.6-1
ARMBIAN_COMMIT=1f6f26d4cfe0ceb9c88e2bbd6f82573546b247a7

work=$(realpath -m "${1:?usage: $0 <work dir> [--install-deps]}")
install_deps=${2:-}
tree=$(git -C "$(dirname "$(realpath "$0")")" rev-parse --show-toplevel)
commit=$(git -C "$tree" rev-parse --short=12 HEAD)
version=$BASE_VERSION+cix$(git -C "$tree" log -1 --format=%cd --date=format:%Y%m%d%H%M HEAD)~resolute

rm -rf "$work"
mkdir -p "$work/src"
git -C "$tree" archive HEAD | tar -x -C "$work/src"
rm -rf "$work/src/ci" "$work/src/.github"
cd "$work/src"

if [ -z "${DEBIAN_DIR:-}" ]; then
	git clone -q --depth 1 --filter=blob:none --no-checkout -b "$DEBIAN_TAG" \
		https://salsa.debian.org/xorg-team/lib/mesa.git "$work/mesa-debian"
	git -C "$work/mesa-debian" sparse-checkout set --no-cone /debian/
	git -C "$work/mesa-debian" checkout -q "$DEBIAN_TAG"
	DEBIAN_DIR=$work/mesa-debian
fi
cp -r "$DEBIAN_DIR/debian" debian

if [ -z "${ARMBIAN_PATCHES:-}" ]; then
	ARMBIAN_PATCHES=$work/armbian-patches
	mkdir -p "$ARMBIAN_PATCHES"
	raw=https://raw.githubusercontent.com/armbian/external/$ARMBIAN_COMMIT/build/mesa/patches
	curl -fsSL "$raw/series" -o "$ARMBIAN_PATCHES/series"
	for p in $(grep -v '^#' "$ARMBIAN_PATCHES/series"); do
		curl -fsSL "$raw/$p" -o "$ARMBIAN_PATCHES/$p"
	done
fi
for p in $(grep -v '^#' "$ARMBIAN_PATCHES/series"); do
	patch -p1 --forward --no-backup-if-mismatch -i "$ARMBIAN_PATCHES/$p"
done
# armbian/resolute carries Debian's quilt patches as commits.
: > debian/patches/series

name=${DEBFULLNAME:-$(git -C "$tree" log -1 --format=%cn HEAD)}
email=${DEBEMAIL:-$(git -C "$tree" log -1 --format=%ce HEAD)}
{
	printf 'mesa (%s) resolute; urgency=medium\n\n' "$version"
	printf '  * mesa-cix %s: Armbian'"'"'s Mesa (mojyack/mesa t210-develop with Debian\n' "$commit"
	printf '    packaging and Armbian'"'"'s packaging patches) plus zink work for the Mali\n'
	printf '    proprietary driver (CIX P1).\n\n'
	printf ' -- %s <%s>  %s\n\n' "$name" "$email" "$(date -R)"
	cat debian/changelog
} > debian/changelog.new
mv debian/changelog.new debian/changelog

if [ "$install_deps" = --install-deps ]; then
	DEBIAN_FRONTEND=noninteractive apt-get build-dep -y --no-install-recommends ./
fi
dpkg-checkbuilddeps
# Debian builds with --wrap-mode=nodownload; Armbian downloads the wraps Debian has no copy of.
meson subprojects download $(ls subprojects/*.wrap | xargs -n1 basename | sed 's/\.wrap$//' | grep -E '^venus-protocol$|-rs$')
# Resolve shared libraries against the distro, not a vendor stack on the linker path.
LD_LIBRARY_PATH=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH) \
DEBIAN_FRONTEND=noninteractive DEB_BUILD_OPTIONS="nocheck parallel=$(nproc) noautodbgsym noddebs" \
	dpkg-buildpackage -b -uc -us > "$work/build.log" 2>&1 || { tail -50 "$work/build.log"; exit 1; }
ls "$work"/*.deb
