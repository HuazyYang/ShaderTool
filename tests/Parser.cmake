if(NOT DEFINED SHADER_TOOL OR NOT DEFINED ALL_BACKENDS_TOOL OR
   NOT DEFINED FAKE_COMPILER OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "parser test paths are required")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(source "${TEST_ROOT}/shader.hlsl")
set(output "${TEST_ROOT}/shader.bin")
file(WRITE "${source}" "// parser fixture\n")

function(expect_failure name expected)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(result EQUAL 0 OR NOT stderr MATCHES "${expected}")
        message(FATAL_ERROR
            "${name}: expected failure /${expected}/, result=${result}\nstdout=${stdout}\nstderr=${stderr}")
    endif()
endfunction()

function(expect_success name)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${name}: expected success, result=${result}\nstdout=${stdout}\nstderr=${stderr}")
    endif()
endfunction()

# Help short-circuits required-option validation. Bare help lists no options;
# backend help lists only the feature groups accepted by that backend.
execute_process(COMMAND "${SHADER_TOOL}" --help RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR stdout MATCHES "ShaderTool control" OR
   stdout MATCHES "compiler executable")
    message(FATAL_ERROR "bare help was not backend-neutral: ${stdout}${stderr}")
endif()
execute_process(COMMAND "${SHADER_TOOL}" dxc --help RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT stdout MATCHES "ShaderTool control" OR
   NOT stdout MATCHES "PATH[ ]+compiler executable" OR
   NOT stdout MATCHES "DXC diagnostics and presentation" OR
   stdout MATCHES "SPIR-V binding remapping" OR stdout MATCHES "fspv-reflect")
    message(FATAL_ERROR "DXC help was not backend-filtered: ${stdout}${stderr}")
endif()
execute_process(COMMAND "${ALL_BACKENDS_TOOL}" fxc --help RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
string(FIND "${stdout}" "NAME[=VALUE]" fxc_value_name)
string(FIND "${stdout}" "define a macro" fxc_description)
if(NOT result EQUAL 0 OR fxc_value_name EQUAL -1 OR
   fxc_description EQUAL -1 OR stdout MATCHES "-HV" OR
   stdout MATCHES "display include hierarchy")
    message(FATAL_ERROR "FXC help was not backend-filtered: ${stdout}${stderr}")
endif()
execute_process(COMMAND "${SHADER_TOOL}" spirv --help RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT stdout MATCHES "SPIR-V binding remapping" OR
   NOT stdout MATCHES "SPIR-V memory layout" OR
   NOT stdout MATCHES "SPIR-V code generation, debug, and optimization" OR
   NOT stdout MATCHES "TYPE NUMBER SPACE SET.*remap a register")
    message(FATAL_ERROR "SPIR-V help lost value metadata: ${stdout}${stderr}")
endif()

# Production native Unix keeps its explicit FXC rejection.
if(NOT WIN32)
    expect_failure(native_fxc "unsupported on native Unix"
        "${SHADER_TOOL}" fxc -compiler "${FAKE_COMPILER}" -T ps_5_0 "${source}")
endif()

# All three backends parse and forward a minimal valid compilation.
expect_success(dxc_backend "${ALL_BACKENDS_TOOL}" dxc -compiler "${FAKE_COMPILER}"
    -T ps_6_0 -Fo "${output}" "${source}")
expect_success(spirv_backend "${ALL_BACKENDS_TOOL}" spirv -compiler "${FAKE_COMPILER}"
    -T ps_6_0 -Fo "${output}" "${source}")
expect_success(fxc_backend "${ALL_BACKENDS_TOOL}" fxc -compiler "${FAKE_COMPILER}"
    -T ps_5_0 -Fo "${output}" "${source}")

# Header output is normalized to a portable byte type and receives a size symbol.
foreach(backend IN ITEMS dxc spirv fxc)
    set(header "${TEST_ROOT}/${backend}.h")
    if(backend STREQUAL fxc)
        set(profile ps_5_0)
    else()
        set(profile ps_6_0)
    endif()
    expect_success(${backend}_header "${ALL_BACKENDS_TOOL}" ${backend}
        -compiler "${FAKE_COMPILER}" -T ${profile} -Fh "${header}"
        -Vn shader_data "${source}")
    file(READ "${header}" header_content)
    if(header_content MATCHES "BYTE" OR
       NOT header_content MATCHES "const unsigned char shader_data\\[\\]" OR
       NOT header_content MATCHES
           "const unsigned int shader_data_size = sizeof\\(shader_data\\);")
        message(FATAL_ERROR "${backend} header was not marshaled:\n${header_content}")
    endif()

    set(default_header "${TEST_ROOT}/${backend}-default.h")
    expect_success(${backend}_default_header "${ALL_BACKENDS_TOOL}" ${backend}
        -compiler "${FAKE_COMPILER}" -T ${profile} -Fh "${default_header}" "${source}")
    file(READ "${default_header}" default_header_content)
    if(NOT default_header_content MATCHES
       "const unsigned int g_main_size = sizeof\\(g_main\\);")
        message(FATAL_ERROR
            "${backend} default header name was not marshaled:\n${default_header_content}")
    endif()

    set(entry_header "${TEST_ROOT}/${backend}-entry.h")
    expect_success(${backend}_entry_header "${ALL_BACKENDS_TOOL}" ${backend}
        -compiler "${FAKE_COMPILER}" -T ${profile} -E CustomEntry
        -Fh "${entry_header}" "${source}")
    file(READ "${entry_header}" entry_header_content)
    if(NOT entry_header_content MATCHES
       "const unsigned int g_CustomEntry_size = sizeof\\(g_CustomEntry\\);")
        message(FATAL_ERROR
            "${backend} entry header name was not marshaled:\n${entry_header_content}")
    endif()
endforeach()

set(precedence_header "${TEST_ROOT}/name-precedence.h")
expect_success(header_name_precedence "${ALL_BACKENDS_TOOL}" dxc
    -compiler "${FAKE_COMPILER}" -T ps_6_0 -Vn ExplicitName -E IgnoredEntry
    -Fh "${precedence_header}" "${source}")
file(READ "${precedence_header}" precedence_header_content)
if(NOT precedence_header_content MATCHES
   "const unsigned int ExplicitName_size = sizeof\\(ExplicitName\\);")
    message(FATAL_ERROR
        "-Vn did not override -E for header naming:\n${precedence_header_content}")
endif()

file(WRITE "${output}" "binary sentinel")
expect_success(binary_output_untouched "${ALL_BACKENDS_TOOL}" dxc
    -compiler "${FAKE_COMPILER}" -T ps_6_0 -Fo "${output}" "${source}")
file(READ "${output}" binary_content)
if(NOT binary_content STREQUAL "binary sentinel")
    message(FATAL_ERROR "-Fo output was marshaled: ${binary_content}")
endif()

# Lookup and arity failures, including multi-value specifications.
expect_failure(unknown_option "unknown or non-compilation option"
    "${SHADER_TOOL}" dxc -unknown)
expect_failure(missing_one_value "option '-compiler' requires 1 value"
    "${SHADER_TOOL}" dxc -compiler)
expect_failure(missing_two_values "option '-fspv-max-id' requires 2 value"
    "${SHADER_TOOL}" spirv -fspv-max-id 1)
expect_failure(missing_four_values "option '-fvk-bind-register' requires 4 value"
    "${SHADER_TOOL}" spirv -fvk-bind-register t 1 0)
expect_success(zero_value_option "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
    -Od -T ps_6_0 -Fo "${output}" "${source}")
expect_success(inline_option "${SHADER_TOOL}" spirv -compiler "${FAKE_COMPILER}"
    -fspv-target-env=vulkan1.2 -T ps_6_0 -Fo "${output}" "${source}")
expect_failure(prefix_requires_equals "unknown or non-compilation option"
    "${SHADER_TOOL}" spirv -fspv-target-env)
expect_failure(exact_option_rejects_suffix "unknown or non-compilation option"
    "${SHADER_TOOL}" dxc -Odextra)

# Values beginning with '-' remain values and preserve their argv boundary.
set(argument_log "${TEST_ROOT}/value-arguments.txt")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "FAKE_ARGUMENT_LOG=${argument_log}"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" -D -NEGATIVE
    -T ps_6_0 -Fo "${output}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dash-prefixed value failed: ${stderr}")
endif()
file(READ "${argument_log}" arguments)
string(FIND "${arguments}" "[-D]\n[-NEGATIVE]" dash_value)
if(dash_value EQUAL -1)
    message(FATAL_ERROR "dash-prefixed value boundary changed: ${arguments}")
endif()

# Native compiler dependency controls are accepted but never forwarded. ShaderTool
# dependency output is selected exclusively with -depfile.
set(argument_log "${TEST_ROOT}/ignored-dependency-arguments.txt")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "FAKE_ARGUMENT_LOG=${argument_log}"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
    -M -MD -MF "${TEST_ROOT}/ignored.d" -Vi
    -T ps_6_0 -Fo "${output}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ignored dependency options failed: ${stderr}")
endif()
file(READ "${argument_log}" arguments)
if(arguments MATCHES "\\[-M\\]" OR arguments MATCHES "\\[-MD\\]" OR
   arguments MATCHES "\\[-MF\\]" OR arguments MATCHES "\\[-Vi\\]" OR
   arguments MATCHES "ignored.d")
    message(FATAL_ERROR "ignored dependency option was forwarded: ${arguments}")
endif()

# Duplicate and required tool-option validation.
expect_failure(duplicate_compiler "-compiler may be specified exactly once"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" -compiler "${FAKE_COMPILER}")
expect_failure(duplicate_depfile "-depfile may be specified at most once"
    "${SHADER_TOOL}" dxc -depfile a -depfile b)
expect_failure(missing_compiler "-compiler is required"
    "${SHADER_TOOL}" dxc -T ps_6_0 "${source}")
expect_failure(missing_profile "-T <profile> is required"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" "${source}")
expect_failure(missing_source "exactly one source file is required"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" -T ps_6_0)
expect_failure(depfile_without_output "-depfile requires a compilation output"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" -T ps_6_0 -depfile dep "${source}")

# Operands are not reordered; the sole source remains final.
expect_failure(source_not_final "exactly one source file must be the final argument"
    "${SHADER_TOOL}" dxc "${source}" -compiler "${FAKE_COMPILER}" -T ps_6_0)
expect_failure(multiple_sources "exactly one source file must be the final argument"
    "${SHADER_TOOL}" dxc first.hlsl second.hlsl)

# Profile and backend policy validation.
expect_failure(invalid_profile "invalid shader profile"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" -T invalid "${source}")
expect_failure(dxc_profile_mismatch "DXC requires shader profile 6_0 or newer"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" -T ps_5_0 "${source}")
expect_failure(fxc_profile_mismatch "FXC requires shader profile 5_0 or 5_1"
    "${ALL_BACKENDS_TOOL}" fxc -compiler "${FAKE_COMPILER}" -T ps_6_0 "${source}")
expect_failure(spirv_only_option "unknown or non-compilation option"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}" -fspv-reflect -T ps_6_0 "${source}")
expect_failure(dxc_only_option_on_fxc "unknown or non-compilation option"
    "${ALL_BACKENDS_TOOL}" fxc -compiler "${FAKE_COMPILER}" -HV 2021 -T ps_5_0 "${source}")
expect_failure(fxc_rejects_presentation "unknown or non-compilation option"
    "${ALL_BACKENDS_TOOL}" fxc -compiler "${FAKE_COMPILER}" -H -T ps_5_0 "${source}")
expect_failure(explicit_spirv_dxc "only selected by the spirv subcommand"
    "${SHADER_TOOL}" dxc -spirv)
expect_failure(explicit_spirv_backend "implicit for the spirv subcommand"
    "${SHADER_TOOL}" spirv -spirv)
expect_failure(explicit_spirv_fxc "not valid for FXC"
    "${ALL_BACKENDS_TOOL}" fxc -spirv)

# Tool-only options are omitted while FXC options are rewritten and retain order.
set(argument_log "${TEST_ROOT}/forwarded-arguments.txt")
set(fxc_depfile "${TEST_ROOT}/fxc.d")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "FAKE_ARGUMENT_LOG=${argument_log}"
    "${ALL_BACKENDS_TOOL}" fxc -compiler "${FAKE_COMPILER}"
    -D FIRST -I SECOND -all-resources-bound -res-may-alias
    -T ps_5_0 -Fo "${output}" -depfile "${fxc_depfile}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "FXC forwarding failed: ${stderr}")
endif()
file(READ "${argument_log}" arguments)
if(NOT arguments MATCHES "\\[/Vi\\]" OR arguments MATCHES "\\[-compiler\\]" OR
   arguments MATCHES "\\[-depfile\\]")
    message(FATAL_ERROR "implicit trace or tool-option omission failed: ${arguments}")
endif()
string(FIND "${arguments}"
    "[/D]\n[FIRST]\n[/I]\n[SECOND]\n[/all_resources_bound]\n[/res_may_alias]\n[/T]\n[ps_5_0]"
    forwarding)
if(forwarding EQUAL -1)
    message(FATAL_ERROR "forwarded option order changed: ${arguments}")
endif()

# DXC/SPIR-V keep original spellings, add their implicit arguments, and preserve
# spaces, quotes, and leading dashes without retokenizing.
set(argument_log "${TEST_ROOT}/spirv-arguments.txt")
set(quoted_define "TEXT=one two\\\"three")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "FAKE_ARGUMENT_LOG=${argument_log}"
    "${SHADER_TOOL}" spirv -compiler "${FAKE_COMPILER}"
    -D "${quoted_define}" -I "${TEST_ROOT}/include path" -D -LEADING
    -T ps_6_0 -Fo "${output}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "SPIR-V exact forwarding failed: ${stderr}")
endif()
file(READ "${argument_log}" arguments)
string(FIND "${arguments}" "[-spirv]" implicit_spirv)
string(FIND "${arguments}" "[-compiler]" forwarded_compiler)
string(FIND "${arguments}" "[${quoted_define}]" preserved_quote)
string(FIND "${arguments}" "[${TEST_ROOT}/include path]" preserved_space)
string(FIND "${arguments}" "[-LEADING]" preserved_dash)
if(implicit_spirv EQUAL -1 OR NOT forwarded_compiler EQUAL -1 OR
   preserved_quote EQUAL -1 OR preserved_space EQUAL -1 OR
   preserved_dash EQUAL -1)
    message(FATAL_ERROR "SPIR-V argv boundaries changed: ${arguments}")
endif()

# Repeated profile/output compiler options remain forwarded; the final output is the depfile target.
set(argument_log "${TEST_ROOT}/repeated-arguments.txt")
set(repeated_depfile "${TEST_ROOT}/repeated.d")
set(first_output "${TEST_ROOT}/first.bin")
set(last_output "${TEST_ROOT}/last.bin")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "FAKE_ARGUMENT_LOG=${argument_log}"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
    -T ps_6_0 -Fo "${first_output}" -T vs_6_1 -Fo "${last_output}"
    -depfile "${repeated_depfile}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "repeated compiler options failed: ${stderr}")
endif()
file(READ "${repeated_depfile}" dependencies)
if(NOT dependencies MATCHES "^${last_output}:")
    message(FATAL_ERROR "last output did not become depfile target: ${dependencies}")
endif()
file(READ "${argument_log}" arguments)
if(NOT arguments MATCHES "\\[-H\\]" OR arguments MATCHES "\\[-compiler\\]" OR
   arguments MATCHES "\\[-depfile\\]")
    message(FATAL_ERROR "DXC implicit trace or tool-option omission failed: ${arguments}")
endif()
string(FIND "${arguments}"
    "[-T]\n[ps_6_0]\n[-Fo]\n[${first_output}]\n[-T]\n[vs_6_1]\n[-Fo]\n[${last_output}]"
    repeated_forwarding)
if(repeated_forwarding EQUAL -1)
    message(FATAL_ERROR "repeated option forwarding changed: ${arguments}")
endif()

# Enough options to force several ParsedOption dynamic-array growth steps.
set(argument_log "${TEST_ROOT}/growing-option-arguments.txt")
set(growing_options)
foreach(index RANGE 0 39)
    list(APPEND growing_options -D "GROW_${index}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "FAKE_ARGUMENT_LOG=${argument_log}"
    "${SHADER_TOOL}" dxc -compiler "${FAKE_COMPILER}"
    ${growing_options} -T ps_6_0 -Fo "${output}" "${source}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "growing option array failed: ${stderr}")
endif()
file(READ "${argument_log}" arguments)
set(previous_position -1)
foreach(index RANGE 0 39)
    string(FIND "${arguments}" "[GROW_${index}]" position)
    if(position EQUAL -1 OR position LESS previous_position)
        message(FATAL_ERROR
            "growing option array lost forwarding order at GROW_${index}: ${arguments}")
    endif()
    set(previous_position ${position})
endforeach()
