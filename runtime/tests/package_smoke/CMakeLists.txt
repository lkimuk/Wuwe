cmake_minimum_required(VERSION 3.20)
project(wuwe_runtime_package_smoke LANGUAGES CXX)

find_package(wuwe 1 CONFIG REQUIRED COMPONENTS runtime_host)

if(NOT TARGET wuwe::runtime_host)
    message(FATAL_ERROR "Installed Wuwe package does not export wuwe::runtime_host")
endif()

add_executable(wuwe_runtime_package_smoke main.cpp)
target_compile_features(wuwe_runtime_package_smoke PRIVATE cxx_std_20)
target_link_libraries(wuwe_runtime_package_smoke PRIVATE wuwe::runtime_host)

enable_testing()
add_test(NAME wuwe_runtime_package_consumer COMMAND wuwe_runtime_package_smoke)
