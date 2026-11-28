#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build Debian packages of this tree the way the mesaaco PPA (ppa:ernstp/mesaaco) builds its Mesa snapshot:
# this tree as upstream, the PPA's own debian/ (taken from its source package), then dpkg-buildpackage -b.
# The PPA's tarball is upstream $MESAACO_COMMIT unchanged, with no Debian patches, so its packaging applies
# as is; the result replaces the PPA's packages one for one.
#
#   ci/build-deb.sh <work dir> [--install-deps]
#
# --install-deps runs "apt-get build-dep" (as root, e.g. in CI); otherwise the build dependencies must
# already be installed (see dpkg-checkbuilddeps). The packaging is fetched unless given:
#   DEBIAN_DIR=<dir with debian/>          the PPA's packaging of $MESAACO_VERSION
# The version is the PPA's plus "+rk<commit date>", so every newer commit sorts higher, and above the
# PPA's own build of the same snapshot (but below its next snapshot: pin the packages). Output:
# <work dir>/*.deb, log in <work dir>/build.log.
set -euo pipefail

# The PPA's Mesa that this branch is based on: update all three when rebasing onto a newer snapshot.
MESAACO_VERSION=26.3.0~git2607311052.b3f90cea886~r~mesaaco0
MESAACO_SHA256=babfd0ee781aad71a0b5fdf6086a746bf2e071db30a64da1d919edeaeeb4f3da
MESAACO_COMMIT=b3f90cea886

work=$(realpath -m "${1:?usage: $0 <work dir> [--install-deps]}")
install_deps=${2:-}
tree=$(git -C "$(dirname "$(realpath "$0")")" rev-parse --show-toplevel)
commit=$(git -C "$tree" rev-parse --short=12 HEAD)
version=$MESAACO_VERSION+rk$(git -C "$tree" log -1 --format=%cd --date=format:%Y%m%d%H%M HEAD)

rm -rf "$work"
mkdir -p "$work/src"
git -C "$tree" archive HEAD | tar -x -C "$work/src"
rm -rf "$work/src/ci" "$work/src/.github"
cd "$work/src"

if [ -z "${DEBIAN_DIR:-}" ]; then
	# The PPA ships a native source package: debian/ is inside the one tarball.
	pool=https://ppa.launchpadcontent.net/ernstp/mesaaco/ubuntu/pool/main/m/mesa
	curl -fsSL --retry 3 "$pool/mesa_${MESAACO_VERSION}.tar.gz" -o "$work/mesaaco.tar.gz"
	echo "$MESAACO_SHA256  $work/mesaaco.tar.gz" | sha256sum -c -
	mkdir -p "$work/mesaaco"
	tar -xzf "$work/mesaaco.tar.gz" -C "$work/mesaaco" --strip-components=1 --wildcards 'mesa/debian/*'
	rm "$work/mesaaco.tar.gz"
	DEBIAN_DIR=$work/mesaaco
fi
cp -r "$DEBIAN_DIR/debian" debian

name=${DEBFULLNAME:-$(git -C "$tree" log -1 --format=%cn HEAD)}
email=${DEBEMAIL:-$(git -C "$tree" log -1 --format=%ce HEAD)}
{
	printf 'mesa (%s) resolute; urgency=medium\n\n' "$version"
	printf '  * mesa-cix %s: the mesaaco snapshot of Mesa (%s)\n' "$commit" "$MESAACO_COMMIT"
	printf '    plus zink work for the Mali proprietary driver (Rockchip libmali g29p1).\n\n'
	printf ' -- %s <%s>  %s\n\n' "$name" "$email" "$(date -R)"
	cat debian/changelog
} > debian/changelog.new
mv debian/changelog.new debian/changelog

if [ "$install_deps" = --install-deps ]; then
	DEBIAN_FRONTEND=noninteractive apt-get build-dep -y --no-install-recommends ./
fi
dpkg-checkbuilddeps
# Resolve shared libraries against the distro, not a vendor stack on the linker path.
LD_LIBRARY_PATH=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH) \
DEBIAN_FRONTEND=noninteractive DEB_BUILD_OPTIONS="nocheck parallel=$(nproc) noautodbgsym noddebs" \
	dpkg-buildpackage -b -uc -us > "$work/build.log" 2>&1 || { tail -50 "$work/build.log"; exit 1; }
ls "$work"/*.deb
