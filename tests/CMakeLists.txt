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
