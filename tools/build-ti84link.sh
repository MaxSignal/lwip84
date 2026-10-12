#!/bin/sh
# build-ti84link.sh - build tools/ti84link.c against libticalcs without root:
# the Ubuntu packages are downloaded with apt-get and unpacked locally.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=${TOOLS_DIR:-$here/../../tools}
debs=$top/debs; root=$top/tilibs; L=$root/usr/lib/x86_64-linux-gnu
mkdir -p "$debs" "$root" "$top/local/bin"
if [ ! -e "$L/libticalcs2.so" ]; then
    (cd "$debs" && apt-get download libticalcs2-13 libtifiles2-11 libticables2-8 libticonv9 \
        libticalcs-dev libtifiles-dev libticables-dev libticonv-dev libgio-2.0-dev)
    for d in "$debs"/*.deb; do dpkg-deb -x "$d" "$root"; done
fi
gcc -O2 -Wall -o "$top/local/bin/ti84link" "$here/ti84link.c" \
    -I"$root/usr/include/tilp2" -I"$root/usr/include/glib-2.0" -I"$L/glib-2.0/include" \
    -L"$L" -Wl,--disable-new-dtags,-rpath,"$L" \
    -lticalcs2 -ltifiles2 -lticables2 -lticonv -l:libglib-2.0.so.0 -l:libarchive.so.13
echo "$top/local/bin/ti84link"
