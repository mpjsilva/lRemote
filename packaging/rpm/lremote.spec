Name:           lremote
Version:        0.1.0
Release:        4%{?dist}
Summary:        mRemoteNG-style remote connections manager built on the Remmina engine
License:        GPL-2.0-or-later
URL:            https://github.com/mpjsilva/lRemote
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake gcc gcc-c++ gettext intltool pkgconfig
BuildRequires:  pkgconfig(gtk+-3.0) pkgconfig(libssh) pkgconfig(vte-2.91)
BuildRequires:  pkgconfig(libsodium) pkgconfig(json-glib-1.0) pkgconfig(libsecret-1)
BuildRequires:  pkgconfig(avahi-ui-gtk3) pkgconfig(ayatana-appindicator3-0.1)
BuildRequires:  pkgconfig(freerdp3) pkgconfig(winpr3) pkgconfig(libvncserver)
BuildRequires:  pkgconfig(spice-client-gtk-3.0) pkgconfig(libcurl) pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(libpcre2-8) pkgconfig(xkbfile) pkgconfig(openssl) pkgconfig(cups)
BuildRequires:  pkgconfig(webkit2gtk-4.1) pkgconfig(gnutls) libgcrypt-devel desktop-file-utils
Source1:        bundle-libs.sh
# Self-contained: every library ships inside the package
AutoReqProv:    no
Requires:       glibc
%global __provides_exclude_from ^/usr/lib64/lremote/.*$
%global debug_package %{nil}
%global __brp_check_rpaths %{nil}

%description
lRemote manages RDP, VNC, SPICE, SSH and web connections in a tree of
folders with a properties panel, like mRemoteNG, using the Remmina
engine. It imports mRemoteNG confCons.xml files and Remmina profiles.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release -DWITH_X2GO=OFF -DWITH_PYTHONLIBS=OFF \
  -DWITH_KF5WALLET=OFF -DWITH_GVNC=OFF -DWITH_WWW=ON -DWITH_FREERDP3=ON
%cmake_build

%install
%cmake_install
mv %{buildroot}%{_bindir}/lremote %{buildroot}%{_libdir}/lremote/lremote-bin
bash %{SOURCE1} %{buildroot}
cat > %{buildroot}%{_bindir}/lremote <<'WRAP'
#!/bin/sh
export LD_LIBRARY_PATH=/usr/lib64/lremote/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export XDG_DATA_DIRS=${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:/usr/lib64/lremote/share
exec /usr/lib64/lremote/lremote-bin "$@"
WRAP
chmod 755 %{buildroot}%{_bindir}/lremote
%find_lang %{name}

%files -f %{name}.lang
%license COPYING
%doc README.md
%{_bindir}/*
%{_libdir}/lremote/
%{_datadir}/lremote/
%{_datadir}/applications/io.github.mpjsilva.lRemote.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.mpjsilva.lRemote.svg
%{_mandir}/man1/*

%changelog
* Mon Nov 16 2026 mpjsilva - 0.1.0-2
- Self-contained package: all libraries and icon themes bundled.
- Fix SPICE plugin failing to load.
* Mon Nov 16 2026 mpjsilva - 0.1.0-1
- First release: mRemoteNG-style window, mRemoteNG and Remmina import, pt_BR.
