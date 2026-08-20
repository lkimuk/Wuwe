execute_process(
    COMMAND "${WUWE_RUNTIME_EXECUTABLE}" doctor --config "${WUWE_RUNTIME_CONFIG}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(result EQUAL 0)
    message(FATAL_ERROR "doctor unexpectedly accepted an invalid ordinary dependency assembly:\n${output}")
endif()

string(JSON assembly_ok ERROR_VARIABLE assembly_error GET "${output}" checks assembly ok)
string(JSON plugins_ok ERROR_VARIABLE plugins_error GET "${output}" checks plugins ok)
string(JSON ready ERROR_VARIABLE ready_error GET "${output}" ready)

if(assembly_error OR plugins_error OR ready_error OR
   assembly_ok OR plugins_ok OR ready)
    message(FATAL_ERROR "doctor did not report the ordinary dependency failure accurately:\n${output}\n${error}")
endif()
