#!/bin/bash
# Copies every shared library lRemote needs into $1/usr/lib64/lremote/lib,
# so the package runs without extra dependencies. Only the C runtime and
# the graphics drivers stay with the system, since they must match the machine.
set -e
ROOT=$1
DEST=$ROOT/usr/lib64/lremote/lib
mkdir -p "$DEST"
SKIP='^(ld-linux|libc\.|libm\.|libdl\.|libpthread\.|librt\.|libresolv\.|libutil\.|libGL|libEGL|libGLX|libGLdispatch|libgbm|libvulkan|libwebkit2gtk|libjavascriptcoregtk)'
BINS="$ROOT/usr/lib64/lremote/lremote-bin $(ls $ROOT/usr/lib64/lremote/plugins/*.so)"
for f in $BINS; do
  # the web plugin keeps using the system WebKitGTK; it is skipped if missing
  case $f in *plugin-www*) continue;; esac
  ldd "$f" | awk '/=> \//{print $3}'
done | sort -u | while read -r lib; do
  base=$(basename "$lib")
  [[ $base =~ $SKIP ]] && continue
  cp -L "$lib" "$DEST/$base"
done
# GTK settings schemas used by dialogs such as the file chooser
mkdir -p "$ROOT/usr/lib64/lremote/share/glib-2.0/schemas"
cp /usr/share/glib-2.0/schemas/org.gtk.Settings.*.xml "$ROOT/usr/lib64/lremote/share/glib-2.0/schemas/"
glib-compile-schemas "$ROOT/usr/lib64/lremote/share/glib-2.0/schemas"
echo "bundled $(ls "$DEST" | wc -l) libraries"
# Icon themes, for systems without GNOME's Adwaita installed
mkdir -p "$ROOT/usr/lib64/lremote/share/icons"
for t in hicolor Adwaita AdwaitaLegacy; do
  [ -d /usr/share/icons/$t ] && cp -r /usr/share/icons/$t "$ROOT/usr/lib64/lremote/share/icons/"
done
rm -f "$ROOT"/usr/lib64/lremote/share/icons/*/icon-theme.cache
for t in "$ROOT"/usr/lib64/lremote/share/icons/*; do gtk-update-icon-cache -q -f "$t" 2>/dev/null || true; done
