if(NOT DEFINED SERENITY OR NOT DEFINED SNINTERP OR NOT DEFINED SOURCE OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "SERENITY, SNINTERP, SOURCE and OUTPUT are required")
endif()

execute_process(
    COMMAND "${SERENITY}" "${SOURCE}" -o "${OUTPUT}" -O2
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "Serenity AOT compilation failed.\n${compile_stdout}\n${compile_stderr}")
endif()

execute_process(
    COMMAND "${OUTPUT}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Generated native executable failed with ${run_result}.\n${run_stdout}\n${run_stderr}")
endif()

execute_process(
    COMMAND "${SNINTERP}" "${SOURCE}"
    RESULT_VARIABLE interpreter_result
    OUTPUT_VARIABLE interpreter_stdout
    ERROR_VARIABLE interpreter_stderr
)
if(NOT interpreter_result EQUAL 0)
    message(FATAL_ERROR "sninterp execution failed with ${interpreter_result}.\n${interpreter_stdout}\n${interpreter_stderr}")
endif()

if(DEFINED EXPECTED_FILE)
    file(READ "${EXPECTED_FILE}" expected)
else()
    set(expected "Serenity\n120\n7\n3.5\n4\nAOT OK\n")
endif()
if(NOT run_stdout STREQUAL expected)
    message(FATAL_ERROR "Unexpected AOT output.\nExpected: [${expected}]\nActual: [${run_stdout}]\n${run_stderr}")
endif()
if(NOT interpreter_stdout STREQUAL expected)
    message(FATAL_ERROR "Unexpected interpreter output.\nExpected: [${expected}]\nActual: [${interpreter_stdout}]\n${interpreter_stderr}")
endif()
if(NOT run_stdout STREQUAL interpreter_stdout)
    message(FATAL_ERROR "AOT and interpreter output differ.\nAOT: [${run_stdout}]\nInterpreter: [${interpreter_stdout}]")
endif()
if(DEFINED SNJIT)
    execute_process(
        COMMAND "${SNJIT}" "${SOURCE}" --hot-threshold 4
        RESULT_VARIABLE jit_result
        OUTPUT_VARIABLE jit_stdout
        ERROR_VARIABLE jit_stderr
    )
    if(NOT jit_result EQUAL 0)
        message(FATAL_ERROR "snjit execution failed with ${jit_result}.\n${jit_stdout}\n${jit_stderr}")
    endif()
    if(NOT jit_stdout STREQUAL expected)
        message(FATAL_ERROR "Unexpected JIT output.\nExpected: [${expected}]\nActual: [${jit_stdout}]\n${jit_stderr}")
    endif()
endif()
