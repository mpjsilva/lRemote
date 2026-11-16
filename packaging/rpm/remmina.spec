%global commit %{?gitcommit}%{!?gitcommit:unknown}

Name:           remmina
Version:        1.4.41
Release:        2.ldmode%{?dist}
Summary:        Remote desktop client (fork with light/dark/system colour scheme)
License:        GPL-2.0-or-later
URL:            https://github.com/mpjsilva/Remmina
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake gcc gcc-c++ gettext intltool pkgconfig
BuildRequires:  pkgconfig(gtk+-3.0) pkgconfig(libssh) pkgconfig(vte-2.91)
BuildRequires:  pkgconfig(libsodium) pkgconfig(json-glib-1.0) pkgconfig(libsecret-1)
BuildRequires:  pkgconfig(avahi-ui-gtk3) pkgconfig(ayatana-appindicator3-0.1)
BuildRequires:  pkgconfig(freerdp3) pkgconfig(winpr3) pkgconfig(libvncserver)
BuildRequires:  pkgconfig(spice-client-gtk-3.0) pkgconfig(libcurl) pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(libpcre2-8) pkgconfig(xkbfile) pkgconfig(openssl) pkgconfig(cups)
BuildRequires:  pkgconfig(webkit2gtk-4.1) pkgconfig(gnutls) libgcrypt-devel desktop-file-utils

Requires:       hicolor-icon-theme

%description
Remmina is a remote desktop client written in GTK, supporting RDP, VNC,
SPICE, SSH and more. This build adds a colour scheme preference with
light, dark and follow-system modes, and a complete pt_BR translation.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release -DREMMINA_GIT_REVISION=%{commit} \
  -DWITH_X2GO=OFF -DWITH_PYTHONLIBS=OFF -DWITH_KF5WALLET=OFF -DWITH_GVNC=OFF -DWITH_WWW=ON -DWITH_FREERDP3=ON
%cmake_build

%install
%cmake_install
%find_lang %{name}

%files -f %{name}.lang
%license COPYING
%doc README.md
%{_bindir}/remmina
%{_bindir}/remmina-file-wrapper
%{_libdir}/remmina/
%{_libdir}/pkgconfig/remmina.pc
%{_includedir}/remmina/
%{_datadir}/remmina/
%{_datadir}/applications/*.desktop
%{_datadir}/icons/hicolor/
%{_datadir}/metainfo/*.xml
%{_datadir}/mime/packages/*.xml
%{_datadir}/man/man1/remmina*.1*

%changelog
* Sun Nov 16 2025 mpjsilva - 1.4.41-2.ldmode
- Light / dark / follow-system colour scheme; complete pt_BR translation.
