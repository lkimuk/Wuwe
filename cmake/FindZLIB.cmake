# Prefer CPR's in-tree zlib-ng target when configuring its bundled curl.
# Falling back to CMake's stock finder preserves normal behavior for all other
# Wuwe configurations.
if(TARGET zlibstatic)
    get_target_property(_WUWE_ZLIB_TARGET zlibstatic ALIASED_TARGET)
    if(NOT _WUWE_ZLIB_TARGET)
        set(_WUWE_ZLIB_TARGET zlibstatic)
    endif()
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
        set_target_properties(ZLIB::ZLIB PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_WUWE_ZLIB_TARGET}")
    endif()
    get_target_property(ZLIB_INCLUDE_DIRS
        ${_WUWE_ZLIB_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
    set(ZLIB_INCLUDE_DIR "${ZLIB_INCLUDE_DIRS}")
    set_target_properties(ZLIB::ZLIB PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIRS}")
    set(ZLIB_LIBRARIES ZLIB::ZLIB)
    set(ZLIB_FOUND TRUE)
    set(ZLIB_VERSION "1.2.13")
    set(ZLIB_VERSION_STRING "1.2.13")
    return()
endif()

include("${CMAKE_ROOT}/Modules/FindZLIB.cmake")
