if(NOT DEFINED SHADER_TOOL OR NOT DEFINED FAKE_COMPILER OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "SHADER_TOOL, FAKE_COMPILER, and TEST_ROOT are required")
endif()
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(source "${TEST_ROOT}/timeout.hlsl")
file(WRITE "${source}" "// timeout\n")
# The production timeout is fixed at six minutes. Keep this standalone test
# bounded while verifying that ShaderTool waits for a delayed compiler and
# preserves its eventual exit status.
string(TIMESTAMP started "%s")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FAKE_SLEEP_MS=100" "FAKE_EXIT_CODE=23"
        "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
        -T ps_6_0 -Fo "${TEST_ROOT}/timeout.bin" "${source}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE stderr)
string(TIMESTAMP finished "%s")
math(EXPR elapsed "${finished} - ${started}")
if(NOT result EQUAL 23)
    message(FATAL_ERROR "delayed compiler exit was ${result}, expected 23: ${stderr}")
endif()
if(elapsed GREATER 3)
    message(FATAL_ERROR "delayed compiler took ${elapsed} seconds")
endif()
