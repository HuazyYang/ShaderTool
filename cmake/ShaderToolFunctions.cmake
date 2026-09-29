# ShaderToolFunctions.cmake
#
# shadertool_add_shader_objects() — turns every row of a ShaderMake-style
# config file into one add_custom_command per requested backend, driven by the
# ShaderTool executable, and gathers all artifacts under one custom target.
#
# DEFINES and DEFINES_DXBC/_DXIL/_SPIRV are GLOBAL defines: they are passed as
# -GD (unkeyed), so they affect compilation of every variant but never enter
# permutation keys and never force a defines-free row into an NVSP blob.
# Per-row -D defines in the config file are the (keyed) permutation axes.
#
# Multi-config generators (Ninja Multi-Config, Visual Studio): the artifacts
# are deliberately config-agnostic — the same .bin/.h path for every
# configuration, matching ShaderMake's behavior. The compile command uses
# $<TARGET_FILE:ShaderTool::ShaderTool>, so whichever configuration builds
# first produces the artifacts and the others reuse them. Do not build the
# same shader target for two configurations inside a single cross-config
# `ninja` invocation.
include_guard(GLOBAL)

set(_SHADERTOOL_ALL_BACKENDS DXBC DXIL SPIRV SLANG-DXBC SLANG-DXIL SLANG-SPIRV)

# Maps a backend to its ShaderTool CLI name and its output class. The class
# (DXBC/DXIL/SPIRV) decides the output subdirectory / header extension: the
# slang backends share dxbc/dxil/spirv with their native counterparts.
function(_shadertool_backend_traits backend cli_variable class_variable)
    if(backend STREQUAL "DXBC")
        set(cli dxbc)
        set(class DXBC)
    elseif(backend STREQUAL "DXIL")
        set(cli dxil)
        set(class DXIL)
    elseif(backend STREQUAL "SPIRV")
        set(cli spirv)
        set(class SPIRV)
    elseif(backend STREQUAL "SLANG-DXBC")
        set(cli slang-dxbc)
        set(class DXBC)
    elseif(backend STREQUAL "SLANG-DXIL")
        set(cli slang-dxil)
        set(class DXIL)
    elseif(backend STREQUAL "SLANG-SPIRV")
        set(cli slang-spirv)
        set(class SPIRV)
    else()
        message(FATAL_ERROR "unknown shader backend: ${backend}")
    endif()
    set(${cli_variable} "${cli}" PARENT_SCOPE)
    set(${class_variable} "${class}" PARENT_SCOPE)
endfunction()

function(shadertool_add_shader_objects)
    set(options
        NO_SPIRV_REGISTER_SHIFTS WARNINGS_AS_ERRORS HLSL_2021 PDB EMBED_PDB
        SLANG_HLSL)
    set(oneValueArgs
        TARGET CONFIG_FILE OUTPUT_MODE OUTPUT_DIRECTORY SHADER_MODEL
        OPTIMIZATION_LEVEL SPIRV_TARGET_ENV VULKAN_MEMORY_LAYOUT MATRIX_LAYOUT
        FOLDER OUTPUT_VARIABLE HEADER_DIRECTORY_VARIABLE)
    set(multiValueArgs
        BACKENDS DEFINES DEFINES_DXBC DEFINES_DXIL DEFINES_SPIRV
        INCLUDE_DIRECTORIES SPIRV_REGISTER_SHIFTS
        SPIRV_EXTENSIONS EXTRA_FLAGS EXTRA_FLAGS_DXBC EXTRA_FLAGS_DXIL
        EXTRA_FLAGS_SPIRV SOURCES)
    cmake_parse_arguments(arg "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # ---- argument validation ----------------------------------------------
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "unknown shadertool_add_shader_objects arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_TARGET OR NOT arg_CONFIG_FILE OR NOT arg_BACKENDS)
        message(FATAL_ERROR
            "shadertool_add_shader_objects requires TARGET, CONFIG_FILE, and "
            "non-empty BACKENDS")
    endif()
    if(TARGET "${arg_TARGET}")
        message(FATAL_ERROR "shader object target already exists: ${arg_TARGET}")
    endif()

    if(NOT arg_OUTPUT_MODE)
        set(arg_OUTPUT_MODE BINARY_BLOB)
    endif()
    if(NOT arg_OUTPUT_MODE STREQUAL "BINARY_BLOB" AND
       NOT arg_OUTPUT_MODE STREQUAL "HEADER_BLOB")
        message(FATAL_ERROR "OUTPUT_MODE must be BINARY_BLOB or HEADER_BLOB, "
            "got ${arg_OUTPUT_MODE}")
    endif()

    if(NOT arg_OUTPUT_DIRECTORY)
        set(arg_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    endif()
    cmake_path(ABSOLUTE_PATH arg_OUTPUT_DIRECTORY NORMALIZE)

    if(NOT arg_SHADER_MODEL)
        set(arg_SHADER_MODEL 6_5)
    endif()
    if(NOT arg_SHADER_MODEL MATCHES "^[0-9]+_[0-9]+$")
        message(FATAL_ERROR "invalid SHADER_MODEL: ${arg_SHADER_MODEL}")
    endif()

    if(DEFINED arg_OPTIMIZATION_LEVEL AND NOT arg_OPTIMIZATION_LEVEL MATCHES "^[0-3]$")
        message(FATAL_ERROR
            "OPTIMIZATION_LEVEL must be 0..3, got ${arg_OPTIMIZATION_LEVEL}")
    endif()
    if(arg_PDB AND arg_EMBED_PDB)
        message(FATAL_ERROR "PDB and EMBED_PDB are mutually exclusive")
    endif()
    if(arg_MATRIX_LAYOUT AND
       NOT arg_MATRIX_LAYOUT MATCHES "^(ROW_MAJOR|COLUMN_MAJOR)$")
        message(FATAL_ERROR
            "MATRIX_LAYOUT must be ROW_MAJOR or COLUMN_MAJOR, got ${arg_MATRIX_LAYOUT}")
    endif()
    if(arg_VULKAN_MEMORY_LAYOUT AND
       NOT arg_VULKAN_MEMORY_LAYOUT MATCHES "^(dx|gl|scalar)$")
        message(FATAL_ERROR
            "VULKAN_MEMORY_LAYOUT must be dx, gl, or scalar, got ${arg_VULKAN_MEMORY_LAYOUT}")
    endif()

    if(NOT DEFINED arg_SPIRV_REGISTER_SHIFTS)
        set(arg_SPIRV_REGISTER_SHIFTS 0 128 256 384)
    endif()
    list(LENGTH arg_SPIRV_REGISTER_SHIFTS spirv_shift_count)
    if(NOT arg_NO_SPIRV_REGISTER_SHIFTS AND NOT spirv_shift_count EQUAL 4)
        message(FATAL_ERROR
            "SPIRV_REGISTER_SHIFTS requires exactly 4 values <t> <s> <b> <u>, "
            "got: ${arg_SPIRV_REGISTER_SHIFTS}")
    endif()
    if(NOT arg_SPIRV_TARGET_ENV)
        set(arg_SPIRV_TARGET_ENV vulkan1.2)
    endif()
    if(NOT DEFINED arg_SPIRV_EXTENSIONS AND
       NOT "SPIRV_EXTENSIONS" IN_LIST arg_KEYWORDS_MISSING_VALUES)
        set(arg_SPIRV_EXTENSIONS SPV_EXT_descriptor_indexing KHR)
    endif()

    # ---- backend selection --------------------------------------------------
    set(selected_backends)
    set(selected_classes)
    foreach(backend IN LISTS arg_BACKENDS)
        string(TOUPPER "${backend}" backend)
        if(NOT backend IN_LIST _SHADERTOOL_ALL_BACKENDS)
            message(FATAL_ERROR "unknown shader backend: ${backend} "
                "(expected one of ${_SHADERTOOL_ALL_BACKENDS})")
        endif()
        if(backend IN_LIST selected_backends)
            message(FATAL_ERROR "duplicate shader backend: ${backend}")
        endif()
        _shadertool_backend_traits("${backend}" backend_cli backend_class)
        # A native backend and its slang twin write to the same subdirectory
        # and files (e.g. DXIL + SLANG-DXIL), so combining them is an error.
        if(backend_class IN_LIST selected_classes)
            message(FATAL_ERROR
                "backends ${arg_BACKENDS} produce colliding ${backend_class} "
                "outputs (e.g. DXIL and SLANG-DXIL cannot be combined)")
        endif()
        list(APPEND selected_backends "${backend}")
        list(APPEND selected_classes "${backend_class}")
    endforeach()

    # ---- global flags (backend-independent) --------------------------------
    set(common_flags)
    # DEFINES are global: they reach every compile as -GD (unkeyed), so they
    # never enter permutation keys or force raw shaders into NVSP blobs.
    foreach(define IN LISTS arg_DEFINES)
        list(APPEND common_flags -GD "${define}")
    endforeach()
    foreach(include_dir IN LISTS arg_INCLUDE_DIRECTORIES)
        cmake_path(ABSOLUTE_PATH include_dir NORMALIZE)
        list(APPEND common_flags -I "${include_dir}")
    endforeach()
    if(DEFINED arg_OPTIMIZATION_LEVEL)
        list(APPEND common_flags "-O${arg_OPTIMIZATION_LEVEL}")
    endif()
    if(arg_WARNINGS_AS_ERRORS)
        list(APPEND common_flags -WX)
    endif()
    if(arg_HLSL_2021)
        list(APPEND common_flags -HV 2021)
    endif()
    if(arg_MATRIX_LAYOUT STREQUAL "ROW_MAJOR")
        list(APPEND common_flags -Zpr)
    elseif(arg_MATRIX_LAYOUT STREQUAL "COLUMN_MAJOR")
        list(APPEND common_flags -Zpc)
    endif()
    if(arg_EMBED_PDB)
        list(APPEND common_flags -Zi -Qembed_debug)
    elseif(arg_PDB)
        list(APPEND common_flags -Zi)
    endif()
    list(APPEND common_flags ${arg_EXTRA_FLAGS})

    # ShaderMake parity: 16-bit types are enabled automatically on SM6-class
    # backends from shader model 6.2 (the tool treats it as a no-op for slang).
    set(enable_16bit_flag)
    string(REGEX MATCH "^([0-9]+)_([0-9]+)$" _sm_match "${arg_SHADER_MODEL}")
    if(CMAKE_MATCH_1 GREATER 6 OR
       (CMAKE_MATCH_1 EQUAL 6 AND CMAKE_MATCH_2 GREATER_EQUAL 2))
        set(enable_16bit_flag -enable-16bit-types)
    endif()

    # Per-output-class global defines, also unkeyed.
    set(class_defines_DXBC)
    set(class_defines_DXIL)
    set(class_defines_SPIRV)
    foreach(class DXBC DXIL SPIRV)
        foreach(define IN LISTS arg_DEFINES_${class})
            list(APPEND class_defines_${class} -GD "${define}")
        endforeach()
    endforeach()

    # ---- SPIR-V flags (SPIRV / SLANG-SPIRV only) ----------------------------
    set(spirv_flags)
    if(NOT arg_NO_SPIRV_REGISTER_SHIFTS)
        list(GET arg_SPIRV_REGISTER_SHIFTS 0 shift_t)
        list(GET arg_SPIRV_REGISTER_SHIFTS 1 shift_s)
        list(GET arg_SPIRV_REGISTER_SHIFTS 2 shift_b)
        list(GET arg_SPIRV_REGISTER_SHIFTS 3 shift_u)
        # Uniform shifts for every stage, register spaces 0..7 (the historic
        # per-stage table is gone on purpose).
        foreach(space RANGE 7)
            list(APPEND spirv_flags
                -fvk-t-shift "${shift_t}" "${space}"
                -fvk-s-shift "${shift_s}" "${space}"
                -fvk-b-shift "${shift_b}" "${space}"
                -fvk-u-shift "${shift_u}" "${space}")
        endforeach()
    endif()
    list(APPEND spirv_flags "-fspv-target-env=${arg_SPIRV_TARGET_ENV}")
    foreach(extension IN LISTS arg_SPIRV_EXTENSIONS)
        list(APPEND spirv_flags "-fspv-extension=${extension}")
    endforeach()
    if(arg_VULKAN_MEMORY_LAYOUT)
        list(APPEND spirv_flags "-fvk-use-${arg_VULKAN_MEMORY_LAYOUT}-layout")
    endif()

    # ---- config file --------------------------------------------------------
    cmake_path(ABSOLUTE_PATH arg_CONFIG_FILE NORMALIZE OUTPUT_VARIABLE config_path)
    if(NOT EXISTS "${config_path}")
        message(FATAL_ERROR "shader config file does not exist: ${config_path}")
    endif()
    cmake_path(GET config_path PARENT_PATH config_dir)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${config_path}")
    file(STRINGS "${config_path}" config_lines)

    set(intermediate_root "${CMAKE_CURRENT_BINARY_DIR}/__shadertool_${arg_TARGET}")
    set(all_outputs)
    set(all_sources)

    foreach(config_line IN LISTS config_lines)
        if(config_line MATCHES "^[ \t]*(#|$)")
            continue()
        endif()
        separate_arguments(row NATIVE_COMMAND "${config_line}")
        list(LENGTH row row_length)
        if(row_length LESS 3)
            message(FATAL_ERROR "invalid shader config row: ${config_line}")
        endif()
        list(GET row 0 source_rel)
        list(REMOVE_AT row 0)

        # Permutation braces are only supported in -D values; a brace anywhere
        # else in a row is a hard configure error.
        if(source_rel MATCHES "[{}]")
            message(FATAL_ERROR
                "braces are only supported in -D values; offending row: ${config_line}")
        endif()
        set(previous_token "")
        foreach(token IN LISTS row)
            if(token MATCHES "[{}]" AND
               NOT previous_token STREQUAL "-D" AND
               NOT token MATCHES "^-D.")
                message(FATAL_ERROR
                    "braces are only supported in -D values; offending row: ${config_line}")
            endif()
            set(previous_token "${token}")
        endforeach()

        # -T <stage> (without shader model; completed per backend below)
        list(FIND row -T profile_index)
        if(profile_index EQUAL -1)
            message(FATAL_ERROR "-T <stage> missing in shader config row: ${config_line}")
        endif()
        math(EXPR profile_value_index "${profile_index} + 1")
        list(LENGTH row row_length)
        if(profile_value_index GREATER_EQUAL row_length)
            message(FATAL_ERROR "-T requires a stage in shader config row: ${config_line}")
        endif()
        list(GET row ${profile_value_index} shader_stage)
        if(NOT shader_stage MATCHES "^[a-z][a-z0-9]*$")
            message(FATAL_ERROR
                "shader config -T value must be an unversioned stage, got "
                "${shader_stage} in row: ${config_line}")
        endif()

        # -E <entry> (default main)
        set(entry_point main)
        list(FIND row -E entry_index)
        if(entry_index GREATER -1)
            math(EXPR entry_value_index "${entry_index} + 1")
            if(entry_value_index GREATER_EQUAL row_length)
                message(FATAL_ERROR
                    "-E requires a value in shader config row: ${config_line}")
            endif()
            list(GET row ${entry_value_index} entry_point)
        endif()

        # Paths: rows are config-file-relative; the compile command runs with
        # WORKING_DIRECTORY = config dir and passes the row's path verbatim.
        cmake_path(ABSOLUTE_PATH source_rel BASE_DIRECTORY "${config_dir}"
            NORMALIZE OUTPUT_VARIABLE source_abs)
        cmake_path(GET source_abs PARENT_PATH source_dir)
        cmake_path(RELATIVE_PATH source_dir BASE_DIRECTORY "${config_dir}"
            OUTPUT_VARIABLE relative_dir)
        if(relative_dir STREQUAL "." OR relative_dir MATCHES "^\\.\\.($|/)")
            set(relative_dir "")
        endif()
        set(relative_prefix "")
        if(relative_dir)
            set(relative_prefix "${relative_dir}/")
        endif()
        cmake_path(GET source_abs STEM LAST_ONLY source_stem)
        set(base_name "${source_stem}")
        if(NOT entry_point STREQUAL "main")
            string(APPEND base_name "_${entry_point}")
        endif()

        foreach(backend IN LISTS selected_backends)
            _shadertool_backend_traits("${backend}" backend_cli backend_class)
            string(TOLOWER "${backend_class}" backend_ext) # dxbc|dxil|spirv

            # DXBC-class backends: shader model pinned to 5_0; library/mesh/
            # amplification stages do not exist there — skip those rows.
            if(backend_class STREQUAL "DXBC")
                if(shader_stage MATCHES "^(lib|ms|as)$")
                    continue()
                endif()
                set(shader_model 5_0)
            else()
                set(shader_model "${arg_SHADER_MODEL}")
            endif()

            set(row_args ${row})
            list(REMOVE_AT row_args ${profile_value_index})
            list(INSERT row_args ${profile_value_index}
                "${shader_stage}_${shader_model}")

            set(backend_flags ${common_flags} ${class_defines_${backend_class}})
            if(backend_class STREQUAL "SPIRV")
                list(APPEND backend_flags ${enable_16bit_flag} ${spirv_flags}
                    ${arg_EXTRA_FLAGS_SPIRV})
            elseif(backend_class STREQUAL "DXIL")
                list(APPEND backend_flags ${enable_16bit_flag} ${arg_EXTRA_FLAGS_DXIL})
            else()
                list(APPEND backend_flags ${arg_EXTRA_FLAGS_DXBC})
            endif()
            if(arg_SLANG_HLSL AND backend MATCHES "^SLANG-")
                list(APPEND backend_flags -lang hlsl -unscoped-enum)
            endif()

            if(arg_OUTPUT_MODE STREQUAL "BINARY_BLOB")
                set(output_dir
                    "${arg_OUTPUT_DIRECTORY}/${backend_ext}/${relative_prefix}")
                set(output_path "${output_dir}${base_name}.bin")
                set(output_flags -Fo "${output_path}")
            else()
                set(output_dir "${arg_OUTPUT_DIRECTORY}/${relative_prefix}")
                set(output_path "${output_dir}${base_name}.${backend_ext}.h")
                string(REPLACE "." "_" symbol_base "${base_name}")
                set(output_flags -Fh "${output_path}"
                    -Vn "g_${symbol_base}_${backend_ext}")
            endif()
            set(depfile_dir "${intermediate_root}/${backend_ext}/${relative_prefix}")
            set(depfile_path "${depfile_dir}${base_name}.d")

            add_custom_command(
                OUTPUT "${output_path}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${output_dir}" "${depfile_dir}"
                COMMAND "$<TARGET_FILE:ShaderTool::ShaderTool>" "${backend_cli}"
                    ${row_args} ${backend_flags} ${output_flags}
                    -depfile "${depfile_path}" "${source_rel}"
                WORKING_DIRECTORY "${config_dir}"
                DEPENDS ShaderTool::ShaderTool "${source_abs}"
                DEPFILE "${depfile_path}"
                COMMENT "ShaderTool [${backend_cli}] ${source_rel} (${shader_stage}_${shader_model})"
                VERBATIM)
            list(APPEND all_outputs "${output_path}")
        endforeach()
        list(APPEND all_sources "${source_abs}")
    endforeach()

    list(REMOVE_DUPLICATES all_sources)
    add_custom_target("${arg_TARGET}"
        DEPENDS ${all_outputs}
        SOURCES "${config_path}" ${arg_SOURCES} ${all_sources})
    if(arg_FOLDER)
        set_target_properties("${arg_TARGET}" PROPERTIES FOLDER "${arg_FOLDER}")
    endif()

    if(arg_OUTPUT_VARIABLE)
        set(${arg_OUTPUT_VARIABLE} "${all_outputs}" PARENT_SCOPE)
    endif()
    if(arg_HEADER_DIRECTORY_VARIABLE)
        if(arg_OUTPUT_MODE STREQUAL "HEADER_BLOB")
            set(${arg_HEADER_DIRECTORY_VARIABLE} "${arg_OUTPUT_DIRECTORY}" PARENT_SCOPE)
        else()
            set(${arg_HEADER_DIRECTORY_VARIABLE} "" PARENT_SCOPE)
        endif()
    endif()
endfunction()
