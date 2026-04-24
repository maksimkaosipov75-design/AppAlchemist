Name:           appalchemist
Version:        1.5.0
Release:        1%{?dist}
Summary:        Convert Linux packages and archives to AppImage format
License:        MIT
URL:            https://github.com/maksimkaosipov75-design/AppAlchemist
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.15
BuildRequires:  qt6-qtbase-devel
BuildRequires:  gcc-c++
BuildRequires:  gtk4-devel
BuildRequires:  libadwaita-devel
BuildRequires:  pkgconf-pkg-config
Requires:       qt6-qtbase
Requires:       gtk4
Requires:       libadwaita

%description
AppAlchemist converts Linux application packages and archives into
portable AppImage bundles through a GTK interface and CLI workflow.

Features:
- GTK4/libadwaita interface for package conversion
- Support for Debian (.deb), RPM (.rpm), and archive inputs
- Automatic dependency analysis and bundling
- Creates portable AppImage files

%prep
%setup -q

%build
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make %{?_smp_mflags}

%install
mkdir -p %{buildroot}%{_bindir}
mkdir -p %{buildroot}%{_libdir}/appalchemist
mkdir -p %{buildroot}%{_datadir}/applications
mkdir -p %{buildroot}%{_datadir}/pixmaps
mkdir -p %{buildroot}%{_datadir}/icons/hicolor

# Install binary
install -m 755 build/appalchemist %{buildroot}%{_bindir}/appalchemist

# Install bundled appimagetool if available
if [ -f ../thirdparty/appimagetool ]; then
    install -m 755 ../thirdparty/appimagetool %{buildroot}%{_libdir}/appalchemist/appimagetool
fi

# Install desktop file
install -m 644 packaging/appalchemist.desktop %{buildroot}%{_datadir}/applications/appalchemist.desktop

# Install icon if available
if [ -f assets/icons/appalchemist.png ]; then
    install -m 644 assets/icons/appalchemist.png %{buildroot}%{_datadir}/pixmaps/appalchemist.png
    for size in 16 32 48 64 128 256; do
        mkdir -p %{buildroot}%{_datadir}/icons/hicolor/${size}x${size}/apps
        install -m 644 assets/icons/appalchemist.png \
            %{buildroot}%{_datadir}/icons/hicolor/${size}x${size}/apps/appalchemist.png
    done
fi

%files
%{_bindir}/appalchemist
%{_libdir}/appalchemist/appimagetool
%{_datadir}/applications/appalchemist.desktop
%{_datadir}/pixmaps/appalchemist.png
%{_datadir}/icons/hicolor/*/apps/appalchemist.png

%changelog
* Fri Apr 24 2026 AppAlchemist Team <appalchemist@example.com> - 1.5.0-1
- Synchronize packaging metadata with the public v1.5.0 release
- Document .deb, .rpm and archive conversion workflows





