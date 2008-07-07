#! /bin/bash

unset LANG LC_ALL LC_COLLATE
export LANG LC_ALL LC_COLLATE

AUTOCONF=${AUTOCONF:-autoconf} \
AUTOMAKE=${AUTOMAKE:-automake} \
ACLOCAL=${ACLOCAL:-aclocal} \
autoreconf -i -f

echo "Now run ./configure"

