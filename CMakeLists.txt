cmake_minimum_required(VERSION 3.22)

project(appalchemist VERSION 1.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# ----------------------------------------------------------------------------
# Build Options
# ----------------------------------------------------------------------------
option(BUILD_CLI "Build standalone command-line converter" ON)
option(BUILD_GUI "Build GTK4/Libadwaita graphical interface" ON)
option(ENABLE_TESTS "Build Catch2 unit and integration tests" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Ensure ~/.local is searched for packages
list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local")

# ----------------------------------------------------------------------------
# Core Dependencies (Removed phantom Qt6::Xml and Qt6::Sql)
# ----------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)

# libarchive for in-process archive extraction
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

# ----------------------------------------------------------------------------
# Core Static Library: appalchemist_core
# ----------------------------------------------------------------------------
set(APPALCHEMIST_CORE_SOURCES
    src/packagetoappimagepipeline.cpp
    src/debparser.cpp
    src/rpmparser.cpp
    src/tarballparser.cpp
    src/dependencyanalyzer.cpp
    src/dependency_resolver.cpp
    src/appdirbuilder.cpp
    src/appimagebuilder.cpp
    src/appdetector.cpp
    src/size_optimizer.cpp
    src/repository_search_worker.cpp
    src/repository_browser.cpp
    src/conversion_controller.cpp
    src/package_extractor.cpp
    src/package_inspector.cpp
    src/package_packager.cpp
    src/package_profile.cpp
    src/compatibility_rules.cpp
    src/runtime_probe.cpp
    src/utils.cpp
    src/archive_extractor.cpp
    src/cache_manager.cpp
)

set(APPALCHEMIST_CORE_HEADERS
    include/packagetoappimagepipeline.h
    include/debparser.h
    include/rpmparser.h
    include/tarballparser.h
    include/dependencyanalyzer.h
    include/dependency_resolver.h
    include/appdirbuilder.h
    include/appimagebuilder.h
    include/appdetector.h
    include/size_optimizer.h
    include/repository_search_worker.h
    include/repository_browser.h
    include/conversion_controller.h
    include/package_extractor.h
    include/package_inspector.h
    include/package_packager.h
    include/package_profile.h
    include/compatibility_rules.h
    include/runtime_probe.h
    include/utils.h
    include/archive_extractor.h
    include/cache_manager.h
)

add_library(appalchemist_core STATIC
    ${APPALCHEMIST_CORE_SOURCES}
    ${APPALCHEMIST_CORE_HEADERS}
)

# ----------------------------------------------------------------------------
# Embedded Assets (Qt Resource System)
# ----------------------------------------------------------------------------
qt_add_resources(appalchemist_core "core_assets"
    PREFIX "/assets"
    BASE "assets"
    FILES
        assets/compatibility_rules.json
        assets/AppRun.template
)

target_include_directories(appalchemist_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(appalchemist_core
    PUBLIC
        Qt6::Core
        Qt6::Network
        PkgConfig::LIBARCHIVE
        PkgConfig::ZSTD
        ZLIB::ZLIB
)

target_compile_definitions(appalchemist_core
    PUBLIC
        HAVE_LIBARCHIVE
        APPALCHEMIST_VERSION="${PROJECT_VERSION}"
)

# Strict Compiler Warnings
if(MSVC)
    target_compile_options(appalchemist_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(appalchemist_core PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

# Sanitizers
if(ENABLE_SANITIZERS)
    target_compile_options(appalchemist_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(appalchemist_core PUBLIC -fsanitize=address,undefined)
endif()

# ----------------------------------------------------------------------------
# Standalone Headless CLI Target: appalchemist-cli (ZERO GTK Dependencies)
# ----------------------------------------------------------------------------
if(BUILD_CLI)
    add_executable(appalchemist-cli
        src/cli/main.cpp
        src/cli/cli_converter.cpp
        include/cli_converter.h
    )

    target_include_directories(appalchemist-cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
    )

    target_compile_definitions(appalchemist-cli PRIVATE
        APPALCHEMIST_VERSION="${PROJECT_VERSION}"
    )

    target_link_libraries(appalchemist-cli PRIVATE
        appalchemist_core
        Qt6::Core
    )

    if(NOT MSVC)
        target_compile_options(appalchemist-cli PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(ENABLE_SANITIZERS)
        target_compile_options(appalchemist-cli PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(appalchemist-cli PUBLIC -fsanitize=address,undefined)
    endif()

    install(TARGETS appalchemist-cli RUNTIME DESTINATION bin)
endif()

# ----------------------------------------------------------------------------
# GTK4 GUI Target: appalchemist-gui
# ----------------------------------------------------------------------------
if(BUILD_GUI)
    pkg_check_modules(GTK_FRONTEND REQUIRED IMPORTED_TARGET gtk4>=4.10 libadwaita-1>=1.5)

    add_executable(appalchemist-gui
        frontend/gtk/main.cpp
        frontend/gtk/app.cpp
        frontend/gtk/app.h
        frontend/gtk/app_window.cpp
        frontend/gtk/app_window.h
    )

    target_include_directories(appalchemist-gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/frontend/gtk
    )

    target_compile_definitions(appalchemist-gui PRIVATE
        APPALCHEMIST_VERSION="${PROJECT_VERSION}"
        APPALCHEMIST_GTK_STYLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/frontend/gtk/style.css"
    )

    target_link_libraries(appalchemist-gui PRIVATE
        appalchemist_core
        PkgConfig::GTK_FRONTEND
        Qt6::Core
    )

    if(NOT MSVC)
        target_compile_options(appalchemist-gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(ENABLE_SANITIZERS)
        target_compile_options(appalchemist-gui PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(appalchemist-gui PUBLIC -fsanitize=address,undefined)
    endif()

    install(TARGETS appalchemist-gui RUNTIME DESTINATION bin)

    # "appalchemist" is the product's command name and is what the desktop
    # entries, MIME handlers and the documentation invoke. The build produces
    # appalchemist-gui and appalchemist-cli, so install a small dispatcher
    # under the historical name that forwards to the right one.
    install(PROGRAMS packaging/appalchemist-dispatch.sh
        DESTINATION bin
        RENAME appalchemist
    )

    # Install desktop files
    install(FILES packaging/appalchemist.desktop
        DESTINATION share/applications
    )
    install(FILES packaging/appalchemist-deb-handler.desktop
        DESTINATION share/applications
    )
    install(FILES packaging/appalchemist-rpm-handler.desktop
        DESTINATION share/applications
    )
endif()

# ----------------------------------------------------------------------------
# Common Assets & Resources Installation
# ----------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icons/appalchemist.png")
    install(FILES assets/icons/appalchemist.png
        DESTINATION share/pixmaps
        RENAME appalchemist.png
    )
    foreach(SIZE 16 32 48 64 128 256)
        install(FILES assets/icons/appalchemist.png
            DESTINATION share/icons/hicolor/${SIZE}x${SIZE}/apps
            RENAME appalchemist.png
        )
    endforeach()
endif()

install(FILES packaging/mime/deb-package.xml
    DESTINATION share/mime/packages
)

install(FILES packaging/mime/rpm-package.xml
    DESTINATION share/mime/packages
)

if(EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/appimagetool")
    install(PROGRAMS thirdparty/appimagetool
        DESTINATION lib/appalchemist
        RENAME appimagetool
    )
endif()

# ----------------------------------------------------------------------------
# Testing Configuration: Catch2 v3 + CTest
# ----------------------------------------------------------------------------
if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
