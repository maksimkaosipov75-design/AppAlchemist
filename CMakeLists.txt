cmake_minimum_required(VERSION 3.15)

project(appalchemist VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Network Xml Sql)
find_package(PkgConfig QUIET)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(APPALCHEMIST_BACKEND_SOURCES
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
    src/cache_manager.cpp
)

set(APPALCHEMIST_BACKEND_HEADERS
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
    include/cache_manager.h
)

add_library(appalchemist_backend STATIC
    ${APPALCHEMIST_BACKEND_SOURCES}
    ${APPALCHEMIST_BACKEND_HEADERS}
)

target_link_libraries(appalchemist_backend
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::Xml
        Qt6::Sql
)

target_compile_definitions(appalchemist_backend
    PRIVATE
        APPALCHEMIST_COMPATIBILITY_RULES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/compatibility_rules.json"
)

if(PkgConfig_FOUND)
    pkg_check_modules(GTK_FRONTEND IMPORTED_TARGET gtk4 libadwaita-1)
endif()

if(GTK_FRONTEND_FOUND)
    add_executable(appalchemist
        frontend/gtk/main.cpp
        frontend/gtk/app.cpp
        frontend/gtk/app.h
        frontend/gtk/app_window.cpp
        frontend/gtk/app_window.h
        src/cli_converter.cpp
        include/cli_converter.h
    )

    target_include_directories(appalchemist
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/frontend/gtk
    )

    target_compile_definitions(appalchemist
        PRIVATE
            APPALCHEMIST_GTK_STYLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/frontend/gtk/style.css"
    )

    target_link_libraries(appalchemist
        PRIVATE
            appalchemist_backend
            PkgConfig::GTK_FRONTEND
            Qt6::Core
    )
else()
    message(FATAL_ERROR "GTK4/libadwaita frontend is required for this build")
endif()

# Install
install(TARGETS appalchemist
    RUNTIME DESTINATION bin
)

# Install desktop file
install(FILES packaging/appalchemist.desktop
    DESTINATION share/applications
)

# Install desktop file handlers
install(FILES packaging/appalchemist-deb-handler.desktop
    DESTINATION share/applications
)

install(FILES packaging/appalchemist-rpm-handler.desktop
    DESTINATION share/applications
)


# Install icon if available
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icons/appalchemist.png")
    install(FILES assets/icons/appalchemist.png
        DESTINATION share/pixmaps
        RENAME appalchemist.png
    )
    # Install to hicolor theme
    foreach(SIZE 16 32 48 64 128 256)
        install(FILES assets/icons/appalchemist.png
            DESTINATION share/icons/hicolor/${SIZE}x${SIZE}/apps
            RENAME appalchemist.png
        )
    endforeach()
endif()

# Install MIME type files
install(FILES packaging/mime/deb-package.xml
    DESTINATION share/mime/packages
)

install(FILES packaging/mime/rpm-package.xml
    DESTINATION share/mime/packages
)

# Install bundled appimagetool if available
if(EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/appimagetool")
    install(PROGRAMS thirdparty/appimagetool
        DESTINATION lib/appalchemist
        RENAME appimagetool
    )
endif()
