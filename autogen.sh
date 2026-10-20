#!/bin/sh
# Generate the GNU autotools build system.
set -e
srcdir=$(dirname "$0")
cd "$srcdir"
mkdir -p m4 build-aux
autoreconf --force --install --verbose
if [ -z "$NOCONFIGURE" ]; then
    exec ./configure "$@"
fi
