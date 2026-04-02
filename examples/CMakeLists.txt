cmake_minimum_required(VERSION 3.14)

project(wuwe_example)

file(GLOB SOURCE_FILES src/*.cpp)
list(LENGTH SOURCE_FILES SRC_FILES_SIZE)
message(STATUS "Found ${SRC_FILES_SIZE} source files of examples")
foreach(source_file ${SOURCE_FILES})
    get_filename_component(file_name ${source_file} NAME)
    string(REPLACE ".cpp" "" name ${file_name})
    message(STATUS " ${name}")
    add_executable(${name} "src/${name}.cpp")
    target_link_libraries(${name} PRIVATE wuwe)
    if(BUILD_SHARED_LIBS)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${name}>
                $<TARGET_FILE_DIR:${name}>
            COMMAND_EXPAND_LISTS
        )
    endif()
endforeach()
