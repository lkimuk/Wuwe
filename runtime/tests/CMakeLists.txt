add_executable(wuwe_runtime_host_tests
    runtime_host_tests.cpp
    header_independence/config_header.cpp
    header_independence/lifecycle_header.cpp
    header_independence/plugin_catalog_header.cpp
    header_independence/plugin_manifest_header.cpp
    header_independence/plugin_package_header.cpp
    header_independence/plugin_process_header.cpp
    header_independence/plugin_runtime_header.cpp
    header_independence/plugin_tools_header.cpp
    header_independence/runtime_host_header.cpp
    header_independence/validation_header.cpp
)

add_executable(wuwe_runtime_plugin_process_fixture plugin_process_fixture.cpp)
set_target_properties(wuwe_runtime_plugin_process_fixture PROPERTIES CXX_EXTENSIONS OFF)
target_compile_features(wuwe_runtime_plugin_process_fixture PRIVATE cxx_std_20)
target_link_libraries(wuwe_runtime_plugin_process_fixture PRIVATE wuwe)
wuwe_runtime_apply_strict_warnings(wuwe_runtime_plugin_process_fixture)
wuwe_copy_runtime_dlls(wuwe_runtime_plugin_process_fixture)

set_target_properties(wuwe_runtime_host_tests PROPERTIES CXX_EXTENSIONS OFF)
target_compile_features(wuwe_runtime_host_tests PRIVATE cxx_std_20)
target_include_directories(wuwe_runtime_host_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(wuwe_runtime_host_tests PRIVATE wuwe::runtime_host)
add_dependencies(wuwe_runtime_host_tests wuwe_runtime_plugin_process_fixture)
target_compile_definitions(wuwe_runtime_host_tests PRIVATE
    WUWE_RUNTIME_PLUGIN_FIXTURE_PATH="$<TARGET_FILE:wuwe_runtime_plugin_process_fixture>"
)
wuwe_runtime_apply_strict_warnings(wuwe_runtime_host_tests)
wuwe_copy_runtime_dlls(wuwe_runtime_host_tests)
add_test(NAME wuwe_runtime_host_tests COMMAND wuwe_runtime_host_tests)
set_tests_properties(wuwe_runtime_host_tests PROPERTIES LABELS concurrency)

add_test(NAME wuwe_runtime_plugin_cli_smoke
    COMMAND $<TARGET_FILE:wuwe-runtime> inspect-plugin
        --package "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/plugin"
)
set_tests_properties(wuwe_runtime_plugin_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "com.example.fixture"
)

if(WUWE_ENABLE_INSTALL)
    add_test(NAME wuwe_runtime_package_smoke
        COMMAND ${CMAKE_COMMAND}
            "-DWUWE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_smoke"
            "-DWUWE_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DWUWE_SMOKE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package_smoke_work"
            "-DWUWE_CONFIGURATION=$<CONFIG>"
            "-DWUWE_GENERATOR=${CMAKE_GENERATOR}"
            "-DWUWE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DWUWE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DWUWE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_package_smoke.cmake"
    )
    set_tests_properties(wuwe_runtime_package_smoke PROPERTIES
        LABELS package
        RUN_SERIAL TRUE
        TIMEOUT 180
    )
endif()
