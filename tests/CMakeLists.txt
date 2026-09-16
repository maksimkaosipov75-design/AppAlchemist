find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# Existing backend unit tests
add_executable(appalchemist_backend_tests backend_tests.cpp)
target_include_directories(appalchemist_backend_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(appalchemist_backend_tests PRIVATE
    appalchemist_core
    Qt6::Core
    PkgConfig::LIBARCHIVE
)
target_compile_definitions(appalchemist_backend_tests PRIVATE HAVE_LIBARCHIVE)
add_test(NAME backend_tests COMMAND appalchemist_backend_tests)

# Catch2 v3 test suite
add_executable(appalchemist_tests
    test_main.cpp
    test_cli.cpp
    test_package_parsers.cpp
    test_e2e_pipeline.cpp
)
target_include_directories(appalchemist_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/cli
)
target_link_libraries(appalchemist_tests PRIVATE
    appalchemist_core
    Catch2::Catch2WithMain
    Qt6::Core
    PkgConfig::LIBARCHIVE
)

if(TARGET appalchemist-cli)
    add_dependencies(appalchemist_tests appalchemist-cli)
    target_compile_definitions(appalchemist_tests PRIVATE
        APPALCHEMIST_CLI_PATH="$<TARGET_FILE:appalchemist-cli>"
    )
endif()

include(CTest)
include(Catch)
catch_discover_tests(appalchemist_tests)
