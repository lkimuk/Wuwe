function(wuwe_copy_runtime_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -Druntime_dlls="$<TARGET_RUNTIME_DLLS:${target}>"
            -Dtarget_dir="$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy-runtime-dlls.cmake"
        COMMAND_EXPAND_LISTS
    )
endfunction()
