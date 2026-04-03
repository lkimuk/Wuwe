file(GLOB_RECURSE SOURCE_FILES "${PROJECT_SOURCE_DIR}/src/*.cpp")

list(LENGTH SOURCE_FILES SRC_FILES_SIZE)
message(STATUS "Found ${SRC_FILES_SIZE} source files")
foreach(source_file ${SOURCE_FILES})
    message(STATUS " ${source_file}")
endforeach()

set(PROJECT_SOURCES
    ${SOURCE_FILES}
)

if(MSVC)
    add_compile_options(/FS)
endif()

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build curl static libraries" FORCE)
set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "Disable libpsl to avoid meson dependency on Windows" FORCE)
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git
                         GIT_TAG f091b2c061b307ee89b164c39976fc9202a1c79d)
FetchContent_MakeAvailable(cpr)

# ---------------------------------------------------------------------------------------
# Static/Shared library
# ---------------------------------------------------------------------------------------
add_library(${PROJECT_NAME} STATIC ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
)

target_link_libraries(${PROJECT_NAME} PRIVATE cpr::cpr)

# target_compile_options(arks PRIVATE)
# target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

# ---------------------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
        EXPORT ${PROJECT_NAME}-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/include/wuwe
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT ${PROJECT_NAME}-targets
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake
)

include(CMakePackageConfigHelpers)
set(LIB_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME})
configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/${PROJECT_NAME}-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake
    PATH_VARS LIB_INSTALL_DIR
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake
)

write_basic_package_version_file(
    "${PROJECT_NAME}-config-version.cmake"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake    
)
