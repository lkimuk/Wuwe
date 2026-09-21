include("${PROJECT_SOURCE_DIR}/cmake/wuwe-runtime.cmake")

add_executable(memory_tests
    memory_tests.cpp
)

target_link_libraries(memory_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(memory_tests)

add_test(NAME memory_tests COMMAND memory_tests)

add_executable(knowledge_tests
    knowledge_tests.cpp
)

target_link_libraries(knowledge_tests PRIVATE wuwe)
if(MSVC)
    target_compile_options(knowledge_tests PRIVATE /bigobj)
endif()
wuwe_copy_runtime_dlls(knowledge_tests)

add_test(NAME knowledge_tests COMMAND knowledge_tests)
set_tests_properties(knowledge_tests PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

add_executable(agent_runtime_tests
    agent_runtime_tests.cpp
)

target_link_libraries(agent_runtime_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(agent_runtime_tests)

add_test(NAME agent_runtime_tests COMMAND agent_runtime_tests)

add_executable(tool_output_projection_tests
    tool_output_projection_tests.cpp
)

target_link_libraries(tool_output_projection_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(tool_output_projection_tests)

add_test(NAME tool_output_projection_tests COMMAND tool_output_projection_tests)

add_executable(sandbox_contract_tests
    sandbox_contract_tests.cpp
)

if(APPLE)
    # DYLD_INSERT_LIBRARIES accepts a dylib, not a loadable bundle.  Keep this
    # probe as a real shared library so the regression test exercises dyld's
    # injection path instead of passing because dyld rejected the fixture.
    add_library(macos_dyld_injection_probe SHARED macos_dyld_injection_probe.cpp)
    set_target_properties(macos_dyld_injection_probe PROPERTIES
        PREFIX ""
        CXX_EXTENSIONS OFF)
    target_compile_features(macos_dyld_injection_probe PRIVATE cxx_std_20)
    target_compile_definitions(sandbox_contract_tests PRIVATE
        WUWE_MACOS_DYLD_INJECTION_PROBE="$<TARGET_FILE:macos_dyld_injection_probe>")
    add_dependencies(sandbox_contract_tests macos_dyld_injection_probe)
endif()

target_link_libraries(sandbox_contract_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(sandbox_contract_tests)

add_test(NAME sandbox_contract_tests COMMAND sandbox_contract_tests)

add_executable(runtime_tests
    runtime_tests.cpp
)

target_link_libraries(runtime_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(runtime_tests)

add_test(NAME runtime_tests COMMAND runtime_tests)

add_executable(runtime_extensions_tests
    runtime_extensions_tests.cpp
)
target_link_libraries(runtime_extensions_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(runtime_extensions_tests)
add_test(NAME runtime_extensions_tests COMMAND runtime_extensions_tests)

add_executable(llm_provider_tests
    llm_provider_tests.cpp
)

target_link_libraries(llm_provider_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(llm_provider_tests)

add_test(NAME llm_provider_tests COMMAND llm_provider_tests)

add_executable(llm_model_discovery_tests llm_model_discovery_tests.cpp)
target_link_libraries(llm_model_discovery_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(llm_model_discovery_tests)
add_test(NAME llm_model_discovery_tests COMMAND llm_model_discovery_tests)

add_executable(p1_infrastructure_tests
    p1_infrastructure_tests.cpp
)
target_link_libraries(p1_infrastructure_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(p1_infrastructure_tests)
add_test(NAME p1_infrastructure_tests COMMAND p1_infrastructure_tests)

add_executable(host_protocol_tests
    host_protocol_tests.cpp
)
target_link_libraries(host_protocol_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(host_protocol_tests)
add_test(NAME host_protocol_tests COMMAND host_protocol_tests)

add_executable(net_tests
    net_tests.cpp
)

target_link_libraries(net_tests PRIVATE wuwe)
if(WUWE_BUILT_WITH_HTTPLIB_HTTPS)
    if(NOT TARGET OpenSSL::SSL)
        find_package(OpenSSL REQUIRED)
    endif()
    target_compile_definitions(net_tests PRIVATE
        CPPHTTPLIB_OPENSSL_SUPPORT=1
        WUWE_NET_TESTS_HAS_OPENSSL=1
        WUWE_NET_TESTS_HAS_HTTPLIB_HTTPS=1
    )
    target_link_libraries(net_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
wuwe_copy_runtime_dlls(net_tests)

add_test(NAME net_tests COMMAND net_tests)

add_executable(planning_tests
    planning_tests.cpp
)

target_link_libraries(planning_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(planning_tests)

add_test(NAME planning_tests COMMAND planning_tests)

add_executable(reflection_tests
    reflection_tests.cpp
)

target_link_libraries(reflection_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(reflection_tests)

add_test(NAME reflection_tests COMMAND reflection_tests)

add_executable(guardrails_tests
    guardrails_tests.cpp
)

target_link_libraries(guardrails_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(guardrails_tests)

add_test(NAME guardrails_tests COMMAND guardrails_tests)

add_executable(evaluation_tests
    evaluation_tests.cpp
)

target_link_libraries(evaluation_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(evaluation_tests)

add_test(NAME evaluation_tests COMMAND evaluation_tests)

add_executable(learning_tests
    learning_tests.cpp
)

target_link_libraries(learning_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(learning_tests)

add_test(NAME learning_tests COMMAND learning_tests)

add_executable(exploration_tests
    exploration_tests.cpp
)

target_link_libraries(exploration_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(exploration_tests)

add_test(NAME exploration_tests COMMAND exploration_tests)

add_executable(routing_tests
    routing_tests.cpp
)

target_link_libraries(routing_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(routing_tests)

add_test(NAME routing_tests COMMAND routing_tests)

add_executable(llm_dispatch_tests
    llm_dispatch_tests.cpp
)

target_link_libraries(llm_dispatch_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(llm_dispatch_tests)

add_test(NAME llm_dispatch_tests COMMAND llm_dispatch_tests)

add_executable(orchestration_tests
    orchestration_tests.cpp
)

target_link_libraries(orchestration_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(orchestration_tests)

add_test(NAME orchestration_tests COMMAND orchestration_tests)

add_executable(best_of_n_tests
    best_of_n_tests.cpp
)

target_link_libraries(best_of_n_tests PRIVATE wuwe)
if(MSVC)
    target_compile_options(best_of_n_tests PRIVATE /bigobj)
endif()
wuwe_copy_runtime_dlls(best_of_n_tests)

add_test(NAME best_of_n_tests COMMAND best_of_n_tests)

add_executable(multi_agent_tests
    multi_agent_tests.cpp
)

target_link_libraries(multi_agent_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(multi_agent_tests)

add_test(NAME multi_agent_tests COMMAND multi_agent_tests)

add_executable(a2a_tests
    a2a_tests.cpp
)

target_link_libraries(a2a_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(a2a_tests)

add_test(NAME a2a_tests COMMAND a2a_tests)

foreach(skills_test
    skills_core_tests
    skills_loader_tests
    skills_activation_tests
    skills_integration_tests)
    add_executable(${skills_test} ${skills_test}.cpp)
    target_link_libraries(${skills_test} PRIVATE wuwe)
    wuwe_copy_runtime_dlls(${skills_test})
    add_test(NAME ${skills_test} COMMAND ${skills_test})
endforeach()

add_executable(header_independence_tests
    header_independence/main.cpp
    header_independence/evaluation_header.cpp
    header_independence/multi_agent_header.cpp
    header_independence/a2a_header.cpp
    header_independence/content_header.cpp
    header_independence/context_token_estimator_header.cpp
    header_independence/execution_context_header.cpp
    header_independence/tool_contract_header.cpp
    header_independence/text_token_estimator_header.cpp
    header_independence/tool_output_projection_header.cpp
    header_independence/tool_output_projection_types_header.cpp
    header_independence/runtime_header.cpp
    header_independence/runtime_extensions_header.cpp
    header_independence/p1_header.cpp
    header_independence/host_header.cpp
    header_independence/filesystem_header.cpp
    header_independence/metadata_header.cpp
    header_independence/sqlite_schema_header.cpp
    header_independence/filesystem_tools_header.cpp
    header_independence/process_tools_header.cpp
    header_independence/sandbox_policy_header.cpp
    header_independence/sandbox_plan_header.cpp
    header_independence/sandbox_codec_header.cpp
    header_independence/llm_dispatch_header.cpp
    header_independence/llm_model_discovery_header.cpp
    header_independence/llm_provider_presets_header.cpp
    header_independence/skills_header.cpp
    header_independence/version_header.cpp
    header_independence/umbrella_header.cpp
)

target_link_libraries(header_independence_tests PRIVATE wuwe)

if(WUWE_ENABLE_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(header_independence_tests PRIVATE /W4 /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(header_independence_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wno-missing-field-initializers)
    endif()
endif()
wuwe_copy_runtime_dlls(header_independence_tests)

add_test(NAME header_independence_tests COMMAND header_independence_tests)

add_executable(reasoning_tests
    reasoning_tests.cpp
)

target_link_libraries(reasoning_tests PRIVATE wuwe)
if(MSVC)
    target_compile_options(reasoning_tests PRIVATE /bigobj)
endif()
wuwe_copy_runtime_dlls(reasoning_tests)

add_test(NAME reasoning_tests COMMAND reasoning_tests)

add_executable(execution_tests
    execution_tests.cpp
)

target_link_libraries(execution_tests PRIVATE wuwe)
if(WIN32)
    target_link_libraries(execution_tests PRIVATE Advapi32 Shell32 Userenv Ws2_32)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" WUWE_EXECUTION_TEST_PYTHON_PATH)
    target_compile_definitions(execution_tests PRIVATE
        WUWE_EXECUTION_TEST_PYTHON="${WUWE_EXECUTION_TEST_PYTHON_PATH}"
    )
endif()
wuwe_copy_runtime_dlls(execution_tests)

add_test(NAME execution_tests COMMAND execution_tests)

add_executable(filesystem_process_tests
    filesystem_process_tests.cpp
)
target_link_libraries(filesystem_process_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(filesystem_process_tests)
add_test(NAME filesystem_process_tests COMMAND filesystem_process_tests)

add_executable(mcp_tests
    mcp_tests.cpp
)

target_link_libraries(mcp_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(mcp_tests)

set(WUWE_MCP_STDIO_TEST_TARGET mcp_stdio_example)
if(NOT TARGET ${WUWE_MCP_STDIO_TEST_TARGET})
    set(WUWE_MCP_STDIO_TEST_TARGET wuwe_mcp_stdio_test_server)
    add_executable(${WUWE_MCP_STDIO_TEST_TARGET}
        "${PROJECT_SOURCE_DIR}/examples/src/mcp_stdio_example.cpp"
    )
    target_link_libraries(${WUWE_MCP_STDIO_TEST_TARGET} PRIVATE wuwe)
    wuwe_copy_runtime_dlls(${WUWE_MCP_STDIO_TEST_TARGET})
endif()

add_dependencies(mcp_tests ${WUWE_MCP_STDIO_TEST_TARGET})
target_compile_definitions(mcp_tests PRIVATE
    WUWE_MCP_STDIO_EXAMPLE_PATH="$<TARGET_FILE:${WUWE_MCP_STDIO_TEST_TARGET}>"
)
add_dependencies(skills_integration_tests ${WUWE_MCP_STDIO_TEST_TARGET})
target_compile_definitions(skills_integration_tests PRIVATE
    WUWE_MCP_STDIO_EXAMPLE_PATH="$<TARGET_FILE:${WUWE_MCP_STDIO_TEST_TARGET}>"
)

add_test(NAME mcp_tests COMMAND mcp_tests)

set_tests_properties(
    knowledge_tests
    planning_tests
    llm_dispatch_tests
    orchestration_tests
    learning_tests
    exploration_tests
    best_of_n_tests
    multi_agent_tests
    a2a_tests
    skills_core_tests
    reasoning_tests
    mcp_tests
    PROPERTIES LABELS "concurrency"
)
