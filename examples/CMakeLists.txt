cmake_minimum_required(VERSION 3.14)

project(wuwe_example)

include("${CMAKE_SOURCE_DIR}/cmake/wuwe-runtime.cmake")

file(GLOB SOURCE_FILES CONFIGURE_DEPENDS src/*.cpp)
list(LENGTH SOURCE_FILES SRC_FILES_SIZE)
message(STATUS "Found ${SRC_FILES_SIZE} source files of examples")
foreach(source_file ${SOURCE_FILES})
    get_filename_component(file_name ${source_file} NAME)
    string(REPLACE ".cpp" "" name ${file_name})
    message(STATUS " ${name}")
    add_executable(${name} "src/${name}.cpp")
    target_link_libraries(${name} PRIVATE wuwe)
    wuwe_copy_runtime_dlls(${name})
endforeach()
