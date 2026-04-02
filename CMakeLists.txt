cmake_minimum_required(VERSION 3.12)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" PROJECT_VERSION_CONTENT)
string(STRIP "${PROJECT_VERSION_CONTENT}" PROJECT_VERSION)
project(wuwe VERSION ${PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (MSVC)
    add_compile_options(/Zc:__cplusplus /permissive-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -pedantic)
endif()

add_subdirectory(src)
add_subdirectory(examples)