cmake_minimum_required(VERSION 3.20)
project(wuwe_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wuwe CONFIG REQUIRED)

add_executable(wuwe_package_smoke main.cpp)
target_link_libraries(wuwe_package_smoke PRIVATE wuwe::wuwe)
