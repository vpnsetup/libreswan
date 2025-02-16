#!/bin/sh

set -xe ; exec < /dev/null

PREFIX=@@PREFIX@@

cat <<EOF | tee /etc/pkg_install.conf
PKG_PATH=http://cdn.NetBSD.org/pub/pkgsrc/packages/NetBSD/i386/10.0/All
EOF

# First install pkgin, it knows how to cache downloaded files.

pkg_add pkgin

# Next hack pkgin's cache to point at /pool

pkgdir=/pool/pkg.netbsd.$(uname -r)
mkdir -p "${pkgdir}"
#rmdir /var/db/pkgin/cache
ln -s "${pkgdir"} /var/db/pkgin/cache

# finally install the packages

: pkg_add git crashes, do not know why
pkgin -y install git
pkgin -y install gmake
pkgin -y install nss
pkgin -y install unbound
pkgin -y install bison
pkgin -y install flex
pkgin -y install ldns
pkgin -y install xmlto
pkgin -y install pkgconf
pkgin -y install fping
pkgin -y install bash
pkgin -y install racoon2
pkgin -y install pkg_developer
pkgin -y install mozilla-rootcerts

# git, for instance, requires the mozilla root certs.
mozilla-rootcerts install

pkg_admin fetch-pkg-vulnerabilities
pkg_admin audit || true
