cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
   (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR
    CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING
        "Minimum supported macOS version" FORCE)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WUWE_PROJECT_VERSION)
string(STRIP "${WUWE_PROJECT_VERSION}" WUWE_PROJECT_VERSION)
project(wuwe VERSION ${WUWE_PROJECT_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WUWE_IS_TOP_LEVEL ON)
else()
    set(WUWE_IS_TOP_LEVEL OFF)
endif()

option(WUWE_BUILD_EXAMPLES "Build Wuwe examples" ${WUWE_IS_TOP_LEVEL})
option(WUWE_BUILD_TESTS "Build Wuwe tests" ${WUWE_IS_TOP_LEVEL})
option(WUWE_BUILD_RUNTIME "Build the Wuwe headless Runtime Host" ${WUWE_IS_TOP_LEVEL})
option(WUWE_ENABLE_INSTALL "Enable Wuwe install and package rules" ${WUWE_IS_TOP_LEVEL})
option(WUWE_ENABLE_STRICT_WARNINGS
    "Compile Wuwe sources with strict warnings treated as errors"
    OFF)
option(WUWE_ENABLE_ADDRESS_SANITIZER
    "Enable AddressSanitizer (and UndefinedBehaviorSanitizer where supported)"
    OFF)
option(WUWE_ENABLE_THREAD_SANITIZER
    "Enable ThreadSanitizer on supported Clang/GCC toolchains"
    OFF)

if(WUWE_ENABLE_ADDRESS_SANITIZER AND WUWE_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR
        "WUWE_ENABLE_ADDRESS_SANITIZER and WUWE_ENABLE_THREAD_SANITIZER are mutually exclusive")
endif()

# Build-only policy targets keep compiler and sanitizer settings scoped to Wuwe.
# They are deliberately excluded from the installed target interface.
add_library(wuwe_project_options INTERFACE)

if(MSVC)
    target_compile_options(wuwe_project_options INTERFACE
        /Zc:__cplusplus
        /permissive-
        /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wuwe_project_options INTERFACE -pedantic)
endif()

if(WUWE_ENABLE_ADDRESS_SANITIZER)
    if(MSVC)
        target_compile_options(wuwe_project_options INTERFACE /fsanitize=address)
        target_link_options(wuwe_project_options INTERFACE /INCREMENTAL:NO)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(wuwe_project_options INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
        target_link_options(wuwe_project_options INTERFACE
            -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR
            "WUWE_ENABLE_ADDRESS_SANITIZER is not supported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(WUWE_ENABLE_THREAD_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        target_compile_options(wuwe_project_options INTERFACE
            -fsanitize=thread
            -fno-omit-frame-pointer)
        target_link_options(wuwe_project_options INTERFACE -fsanitize=thread)
    else()
        message(FATAL_ERROR
            "WUWE_ENABLE_THREAD_SANITIZER requires a supported Clang or GCC toolchain")
    endif()
endif()

set(WUWE_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${WUWE_GENERATED_INCLUDE_DIR}/wuwe")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/wuwe/version.hpp.in"
    "${WUWE_GENERATED_INCLUDE_DIR}/wuwe/version.hpp"
    @ONLY)

add_subdirectory(src)

if(WUWE_BUILD_TESTS)
    enable_testing()
endif()

if(WUWE_BUILD_RUNTIME)
    add_subdirectory(runtime)
endif()

if(WUWE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(WUWE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
