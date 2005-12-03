#! /bin/bash

unset LANG LC_ALL LC_COLLATE
export LANG LC_ALL LC_COLLATE

AUTOCONF=${AUTOCONF:-autoconf} \
AUTOMAKE=${AUTOMAKE:-automake-1.9} \
ACLOCAL=${ACLOCAL:-aclocal-1.9} \
autoreconf -i -f

echo "Now run ./configure"

