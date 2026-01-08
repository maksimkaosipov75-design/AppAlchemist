Name:           appalchemist
Version:        1.0.0
Release:        1%{?dist}
Summary:        Convert .deb and .rpm packages to AppImage format
License:        MIT
URL:            https://github.com/appalchemist/appalchemist
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.15
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  gcc-c++
Requires:       qt6-qtbase

%description
AppAlchemist is a graphical application that converts Linux package
formats (.deb and .rpm) into self-contained AppImage files that can
run on any Linux distribution without installation.

Features:
- Drag-and-drop interface for easy conversion
- Support for both Debian (.deb) and RPM (.rpm) packages
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
* Mon Jan 05 2025 AppAlchemist Team <appalchemist@example.com> - 1.0.0-1
- Initial release
- Support for .deb and .rpm package conversion
- Graphical user interface with drag-and-drop
- Automatic dependency bundling






