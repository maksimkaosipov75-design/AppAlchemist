cmake_minimum_required(VERSION 3.15)

project(appalchemist VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# Enable Qt MOC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Source files
set(APPALCHEMIST_SOURCES
    src/main.cpp
    src/appmainwindow.cpp
    src/packagetoappimagepipeline.cpp
    src/debparser.cpp
    src/rpmparser.cpp
    src/dependencyanalyzer.cpp
    src/appdirbuilder.cpp
    src/appimagebuilder.cpp
    src/appdetector.cpp
    src/utils.cpp
    src/cache_manager.cpp
    src/cli_converter.cpp
)

set(APPALCHEMIST_HEADERS
    include/appmainwindow.h
    include/packagetoappimagepipeline.h
    include/debparser.h
    include/rpmparser.h
    include/dependencyanalyzer.h
    include/appdirbuilder.h
    include/appimagebuilder.h
    include/appdetector.h
    include/utils.h
    include/cache_manager.h
    include/cli_converter.h
)

set(APPALCHEMIST_UI
    ui/appmainwindow.ui
)

# Create executable
add_executable(appalchemist
    ${APPALCHEMIST_SOURCES}
    ${APPALCHEMIST_HEADERS}
    ${APPALCHEMIST_UI}
)

# Link Qt libraries
target_link_libraries(appalchemist
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

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



