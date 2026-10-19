if(NOT DEFINED POWERSHELL_EXECUTABLE OR NOT DEFINED ROOT_DIRECTORY)
    message(FATAL_ERROR "POWERSHELL_EXECUTABLE and ROOT_DIRECTORY are required.")
endif()

execute_process(
    COMMAND "${POWERSHELL_EXECUTABLE}"
        -NoProfile
        -ExecutionPolicy Bypass
        -File "${ROOT_DIRECTORY}/scripts/check_source_style.ps1"
        -root "${ROOT_DIRECTORY}"
    RESULT_VARIABLE style_result
    COMMAND_ECHO STDOUT)
if(NOT style_result EQUAL 0)
    message(FATAL_ERROR "Source style check failed with exit code: ${style_result}")
endif()

