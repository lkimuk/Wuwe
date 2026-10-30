cmake_minimum_required(VERSION 3.20)
project(wuwe_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wuwe 1 CONFIG REQUIRED)

get_target_property(WUWE_REQUIRED_CXX_FEATURES wuwe::wuwe INTERFACE_COMPILE_FEATURES)
if(NOT "cxx_std_20" IN_LIST WUWE_REQUIRED_CXX_FEATURES)
    message(FATAL_ERROR "Installed Wuwe target does not publish its C++20 requirement")
endif()

add_executable(wuwe_package_smoke main.cpp)
target_link_libraries(wuwe_package_smoke PRIVATE wuwe::wuwe)

if(MSVC)
    # Catch mixed Debug/Release CRT dependencies in the installed package.
    target_link_options(wuwe_package_smoke PRIVATE /WX)
endif()
