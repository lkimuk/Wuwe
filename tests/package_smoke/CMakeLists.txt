cmake_minimum_required(VERSION 3.20)
project(wuwe_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wuwe CONFIG REQUIRED)

function(wuwe_copy_runtime_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target}>
            $<TARGET_FILE_DIR:${target}>
        COMMAND_EXPAND_LISTS
    )
endfunction()

add_executable(wuwe_package_smoke main.cpp)
target_link_libraries(wuwe_package_smoke PRIVATE wuwe::wuwe)
wuwe_copy_runtime_dlls(wuwe_package_smoke)
