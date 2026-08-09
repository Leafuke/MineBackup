if(NOT DEFINED GUI)
    message(FATAL_ERROR "GUI is required")
endif()

function(expect_special_disabled)
    execute_process(
        COMMAND "${GUI}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 10)
    if(NOT result EQUAL 2)
        message(FATAL_ERROR "deprecated desktop special execution should exit 2, got ${result}")
    endif()
    string(FIND "${error}" "minebackup-cli" hint)
    if(hint EQUAL -1)
        message(FATAL_ERROR "deprecated desktop special execution did not write the CLI replacement to stderr: ${error}")
    endif()
endfunction()

expect_special_disabled(--run-special special-id)
expect_special_disabled(-specialcfg 1)
expect_special_disabled(-specialcfg invalid)
