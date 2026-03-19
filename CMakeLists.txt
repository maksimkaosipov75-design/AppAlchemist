cmake_minimum_required(VERSION 3.15)

project(appalchemist VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Xml Sql)
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
    include/utils.h
    include/cache_manager.h
)

set(APPALCHEMIST_QT_GUI_SOURCES
    src/main.cpp
    src/appmainwindow.cpp
    src/cli_converter.cpp
    src/search_dialog.cpp
    src/appstore_window.cpp
    src/appstream_metadata.cpp
    src/appstore_model.cpp
    src/app_card_widget.cpp
    src/app_details_dialog.cpp
    src/store/store_repository.cpp
    src/store/store_indexer.cpp
    src/store/store_cache.cpp
    src/store/store_ratings.cpp
    src/store/store_viewmodel.cpp
    src/store/store_app_card.cpp
    src/store/store_details_dialog.cpp
)

set(APPALCHEMIST_QT_GUI_HEADERS
    include/appmainwindow.h
    include/cli_converter.h
    include/search_dialog.h
    include/appstore_window.h
    include/appstream_metadata.h
    include/appstore_model.h
    include/app_card_widget.h
    include/app_details_dialog.h
    include/store/store_appentry.h
    include/store/store_repository.h
    include/store/store_indexer.h
    include/store/store_cache.h
    include/store/store_ratings.h
    include/store/store_viewmodel.h
    include/store/store_app_card.h
    include/store/store_details_dialog.h
)

set(APPALCHEMIST_UI
    ui/appmainwindow.ui
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

add_executable(appalchemist
    ${APPALCHEMIST_QT_GUI_SOURCES}
    ${APPALCHEMIST_QT_GUI_HEADERS}
    ${APPALCHEMIST_UI}
)

target_link_libraries(appalchemist
    PRIVATE
        appalchemist_backend
        Qt6::Widgets
)

if(PkgConfig_FOUND)
    pkg_check_modules(GTK_FRONTEND IMPORTED_TARGET gtk4 libadwaita-1)
endif()

if(GTK_FRONTEND_FOUND)
    add_executable(appalchemist-gtk
        frontend/gtk/main.cpp
        frontend/gtk/app.cpp
        frontend/gtk/app.h
        frontend/gtk/app_window.cpp
        frontend/gtk/app_window.h
    )

    target_include_directories(appalchemist-gtk
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/frontend/gtk
    )

    target_compile_definitions(appalchemist-gtk
        PRIVATE
            APPALCHEMIST_GTK_STYLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/frontend/gtk/style.css"
    )

    target_link_libraries(appalchemist-gtk
        PRIVATE
            appalchemist_backend
            PkgConfig::GTK_FRONTEND
    )
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
        DESTINATION usr/lib/appalchemist
        RENAME appimagetool
    )
endif()
