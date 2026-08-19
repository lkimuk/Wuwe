include("${PROJECT_SOURCE_DIR}/cmake/wuwe-runtime.cmake")

function(wuwe_runtime_apply_strict_warnings target)
    if(NOT WUWE_ENABLE_STRICT_WARNINGS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)
    endif()
endfunction()

add_library(wuwe_runtime_host STATIC
    src/config.cpp
    src/lifecycle.cpp
    src/plugin_manifest.cpp
    src/runtime_host.cpp
    src/validation.cpp
)
add_library(wuwe::runtime_host ALIAS wuwe_runtime_host)

set_target_properties(wuwe_runtime_host PROPERTIES
    OUTPUT_NAME wuwe-runtime-host
    EXPORT_NAME runtime_host
    DEBUG_POSTFIX d
    CXX_EXTENSIONS OFF
)
target_compile_features(wuwe_runtime_host PUBLIC cxx_std_20)
target_include_directories(wuwe_runtime_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(wuwe_runtime_host
    PUBLIC wuwe
    PRIVATE "$<BUILD_INTERFACE:wuwe_project_options>"
)

if(MSVC)
    target_compile_options(wuwe_runtime_host PRIVATE /FS)
endif()

wuwe_runtime_apply_strict_warnings(wuwe_runtime_host)

add_executable(wuwe-runtime app/main.cpp)
set_target_properties(wuwe-runtime PROPERTIES CXX_EXTENSIONS OFF)
target_compile_features(wuwe-runtime PRIVATE cxx_std_20)
target_link_libraries(wuwe-runtime PRIVATE wuwe::runtime_host)
wuwe_runtime_apply_strict_warnings(wuwe-runtime)
wuwe_copy_runtime_dlls(wuwe-runtime)

if(WUWE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(WUWE_ENABLE_INSTALL)
    install(TARGETS wuwe_runtime_host
        EXPORT wuwe-runtime-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
        install(TARGETS wuwe-runtime
            RUNTIME_DEPENDENCY_SET wuwe-runtime-dependencies
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        install(RUNTIME_DEPENDENCY_SET wuwe-runtime-dependencies
            PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
            POST_EXCLUDE_REGEXES
                ".*[Ww]indows[/\\\\][Ss]ystem32[/\\\\].*"
                "^/System/Library/.*"
                "^/usr/lib/.*"
                "^/lib/.*"
                "^/lib64/.*"
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    else()
        install(TARGETS wuwe-runtime
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        if(WIN32 AND WUWE_BUILT_WITH_SQLITE)
            if(WUWE_SQLITE_CMAKE_TARGET AND TARGET "${WUWE_SQLITE_CMAKE_TARGET}")
                install(FILES "$<TARGET_FILE:${WUWE_SQLITE_CMAKE_TARGET}>"
                    DESTINATION ${CMAKE_INSTALL_BINDIR})
            elseif(TARGET unofficial::sqlite3::sqlite3)
                install(FILES "$<TARGET_FILE:unofficial::sqlite3::sqlite3>"
                    DESTINATION ${CMAKE_INSTALL_BINDIR})
            endif()
        endif()
    endif()
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/wuwe"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime.example.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wuwe/runtime
    )
    install(EXPORT wuwe-runtime-targets
        FILE wuwe-runtime-targets.cmake
        NAMESPACE wuwe::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/wuwe/cmake
    )
endif()
