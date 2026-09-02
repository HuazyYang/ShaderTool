if(NOT DEFINED SHADER_TOOL OR NOT DEFINED FAKE_COMPILER OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "SHADER_TOOL, FAKE_COMPILER, and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/with space")
set(source "${TEST_ROOT}/source file.hlsl")
set(include "${TEST_ROOT}/with space/include file.hlsli")
set(output "${TEST_ROOT}/output file.bin")
set(depfile "${TEST_ROOT}/output file.d")
set(argument_log "${TEST_ROOT}/arguments.txt")
file(WRITE "${source}" "// source\n")
file(WRITE "${include}" "// include\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FAKE_INCLUDE=${include}"
        "FAKE_ARGUMENT_LOG=${argument_log}"
        "FAKE_OUTPUT_BYTES=131072"
        "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
        -D "VALUE=a b" -T ps_6_0 -Fo "${output}" -depfile "${depfile}" "${source}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "large-output compile failed (${result}):\n${stdout}\n${stderr}")
endif()
file(READ "${depfile}" dependencies)
set(expected "${output}: ${source} ${include}\n")
if(NOT dependencies STREQUAL expected)
    message(FATAL_ERROR "unexpected depfile:\n${dependencies}\nexpected:\n${expected}")
endif()
file(READ "${argument_log}" arguments)
string(FIND "${arguments}" "[-D]\n[VALUE=a b]" argument_boundary)
if(argument_boundary EQUAL -1)
    message(FATAL_ERROR "argument boundary was not preserved:\n${arguments}")
endif()

file(TIMESTAMP "${depfile}" timestamp_before "%s")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "FAKE_INCLUDE=${include}"
        "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
        -T ps_6_0 -Fo "${output}" -depfile "${depfile}" "${source}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "unchanged depfile compile failed (${result}): ${stderr}")
endif()
file(TIMESTAMP "${depfile}" timestamp_after "%s")
if(NOT timestamp_before STREQUAL timestamp_after)
    message(FATAL_ERROR "unchanged depfile timestamp changed")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "FAKE_INCLUDE=${include}"
        "${SHADER_TOOL}" spirv -compiler "${FAKE_COMPILER}"
        -T ps_6_0 -Fo "${output}" -depfile - "${source}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "stdout depfile compile failed (${result}): ${stderr}")
endif()
if(NOT stdout STREQUAL expected)
    message(FATAL_ERROR "-depfile - stdout was not pure:\n${stdout}\nexpected:\n${expected}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "FAKE_EXIT_CODE=23"
        "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
        -T ps_6_0 -Fo "${output}" "${source}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(NOT result EQUAL 23)
    message(FATAL_ERROR "compiler exit code was ${result}, expected 23")
endif()

execute_process(
    COMMAND "${SHADER_TOOL}" dxc -compiler "${TEST_ROOT}/missing-compiler"
        -T ps_6_0 -Fo "${output}" "${source}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE stderr)
if(result EQUAL 0 OR NOT stderr MATCHES "ShaderTool:.*failed")
    message(FATAL_ERROR "missing compiler diagnostic was not contextual: ${stderr}")
endif()
