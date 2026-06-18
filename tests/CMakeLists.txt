include("${CMAKE_SOURCE_DIR}/cmake/wuwe-runtime.cmake")

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
wuwe_copy_runtime_dlls(knowledge_tests)

add_test(NAME knowledge_tests COMMAND knowledge_tests)
set_tests_properties(knowledge_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(agent_runtime_tests
    agent_runtime_tests.cpp
)

target_link_libraries(agent_runtime_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(agent_runtime_tests)

add_test(NAME agent_runtime_tests COMMAND agent_runtime_tests)

add_executable(llm_provider_tests
    llm_provider_tests.cpp
)

target_link_libraries(llm_provider_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(llm_provider_tests)

add_test(NAME llm_provider_tests COMMAND llm_provider_tests)

add_executable(net_tests
    net_tests.cpp
)

target_link_libraries(net_tests PRIVATE wuwe)
if(NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL QUIET)
endif()
if(TARGET OpenSSL::SSL)
    target_compile_definitions(net_tests PRIVATE
        CPPHTTPLIB_OPENSSL_SUPPORT=1
        WUWE_NET_TESTS_HAS_OPENSSL=1
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

add_executable(reasoning_tests
    reasoning_tests.cpp
)

target_link_libraries(reasoning_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(reasoning_tests)

add_test(NAME reasoning_tests COMMAND reasoning_tests)

add_executable(execution_tests
    execution_tests.cpp
)

target_link_libraries(execution_tests PRIVATE wuwe)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    target_compile_definitions(execution_tests PRIVATE
        WUWE_EXECUTION_TEST_PYTHON="${Python3_EXECUTABLE}"
    )
endif()
wuwe_copy_runtime_dlls(execution_tests)

add_test(NAME execution_tests COMMAND execution_tests)

add_executable(mcp_tests
    mcp_tests.cpp
)

target_link_libraries(mcp_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(mcp_tests)
add_dependencies(mcp_tests mcp_stdio_example)
target_compile_definitions(mcp_tests PRIVATE
    WUWE_MCP_STDIO_EXAMPLE_PATH="$<TARGET_FILE:mcp_stdio_example>"
)

add_test(NAME mcp_tests COMMAND mcp_tests)
