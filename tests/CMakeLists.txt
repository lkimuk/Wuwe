add_executable(memory_tests
    memory_tests.cpp
)

target_link_libraries(memory_tests PRIVATE wuwe)

add_custom_command(TARGET memory_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:memory_tests>
        $<TARGET_FILE_DIR:memory_tests>
    COMMAND_EXPAND_LISTS
)

add_test(NAME memory_tests COMMAND memory_tests)
