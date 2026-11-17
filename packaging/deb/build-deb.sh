#!/bin/bash
# Builds a self-contained lremote .deb (Ubuntu 24.04 / Debian 13 or newer).
# Run inside the remmina-build image:  bash packaging/deb/build-deb.sh <release> <outdir>
set -e
REL=${1:-1}; OUT=${2:-dist}
VER=$(sed -n 's/^set(LREMOTE_VERSION "\(.*\)")/\1/p' CMakeLists.txt); VER=${VER:-0.1.0}
B=/tmp/lremote-deb-build; R=/tmp/lremote-deb-root
rm -rf $B $R
cmake -S . -B $B -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib64 \
  -DWITH_X2GO=OFF -DWITH_PYTHONLIBS=OFF -DWITH_KF5WALLET=OFF -DWITH_GVNC=OFF -DWITH_WWW=ON \
  -DWITH_FREERDP3=ON -DLREMOTE_RELEASE=$REL >/dev/null
cmake --build $B -j"$(nproc)" >/dev/null
DESTDIR=$R cmake --install $B >/dev/null
mv $R/usr/bin/lremote $R/usr/lib64/lremote/lremote-bin
bash packaging/rpm/bundle-libs.sh $R
cat > $R/usr/bin/lremote <<'WRAP'
#!/bin/sh
export LD_LIBRARY_PATH=/usr/lib64/lremote/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export XDG_DATA_DIRS=${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:/usr/lib64/lremote/share
exec /usr/lib64/lremote/lremote-bin "$@"
WRAP
chmod 755 $R/usr/bin/lremote
rm -rf $R/usr/include $R/usr/lib64/pkgconfig
mkdir -p $R/DEBIAN
cat > $R/DEBIAN/control <<CTL
Package: lremote
Version: $VER-$REL
Architecture: amd64
Maintainer: mpjsilva <https://github.com/mpjsilva>
Depends: libc6 (>= 2.39), fontconfig
Recommends: libwebkit2gtk-4.1-0
Section: net
Priority: optional
Installed-Size: $(du -sk $R/usr | cut -f1)
Homepage: https://github.com/mpjsilva/lRemote
Description: Remote connection manager in the style of mRemoteNG
 lRemote brings the mRemoteNG workflow to Linux on top of the Remmina
 engine: connection tree, tabs, RDP, VNC, SSH, SPICE and mRemoteNG import.
 All libraries are bundled; only the C runtime comes from the system.
CTL
mkdir -p $OUT
dpkg-deb --root-owner-group -Zxz --build $R "$OUT/lremote_${VER}-${REL}_amd64.deb"
