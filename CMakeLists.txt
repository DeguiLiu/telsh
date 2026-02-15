cmake_minimum_required(VERSION 3.14)
project(telsh LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library
add_library(telsh INTERFACE)
target_include_directories(telsh INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(telsh INTERFACE pthread)

# Example
option(TELSH_BUILD_EXAMPLES "Build examples" ON)
if(TELSH_BUILD_EXAMPLES)
    add_executable(telsh_example examples/main.cpp)
    target_link_libraries(telsh_example PRIVATE telsh)
endif()

# Tests
option(TELSH_BUILD_TESTS "Build tests" ON)
if(TELSH_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    set(CATCH2_VERSION v3.5.2)
    FetchContent_Declare(
        Catch2
        URL https://ghfast.top/https://github.com/catchorg/Catch2/archive/refs/tags/${CATCH2_VERSION}.zip
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    add_executable(telsh_tests
        tests/test_command_registry.cpp
        tests/test_telnet_session.cpp
    )
    target_link_libraries(telsh_tests PRIVATE telsh Catch2::Catch2WithMain)
    catch_discover_tests(telsh_tests
        PROPERTIES SKIP_RETURN_CODE 4
    )
endif()
