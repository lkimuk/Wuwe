execute_process(
    COMMAND "${WUWE_RUNTIME_EXECUTABLE}" run --config "${WUWE_RUNTIME_CONFIG}" --prompt test
    RESULT_VARIABLE rejected_result
    OUTPUT_VARIABLE rejected_output
    ERROR_VARIABLE rejected_error
)

if(NOT rejected_result EQUAL 2 OR
   NOT rejected_error MATCHES "requires --approve-tools")
    message(FATAL_ERROR
        "plugin-enabled run did not require explicit approval:\n${rejected_output}\n${rejected_error}")
endif()

execute_process(
    COMMAND "${WUWE_RUNTIME_EXECUTABLE}" run --config "${WUWE_RUNTIME_CONFIG}"
        --prompt test --approve-tools
    RESULT_VARIABLE approved_result
    OUTPUT_VARIABLE approved_output
    ERROR_VARIABLE approved_error
)

# This fixture intentionally has an invalid SQLite path. Reaching startup failure proves the
# approval flag was accepted rather than treated as an implicit or unknown command-line option.
if(NOT approved_result EQUAL 4 OR approved_error MATCHES "requires --approve-tools")
    message(FATAL_ERROR
        "explicit plugin approval did not reach Runtime startup:\n${approved_output}\n${approved_error}")
endif()
