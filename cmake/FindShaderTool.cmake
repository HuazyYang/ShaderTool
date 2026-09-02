include_guard(GLOBAL)

include(FindPackageHandleStandardArgs)

if(NOT TARGET ShaderTool)
    if(ShaderTool_ROOT)
        cmake_path(ABSOLUTE_PATH ShaderTool_ROOT NORMALIZE
            OUTPUT_VARIABLE _ShaderTool_source_dir)
    else()
        cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH _ShaderTool_source_dir)
    endif()

    if(EXISTS "${_ShaderTool_source_dir}/CMakeLists.txt" AND
       EXISTS "${_ShaderTool_source_dir}/ShaderTool.c")
        if(NOT ShaderTool_BINARY_DIR)
            set(ShaderTool_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/ShaderTool")
        endif()
        add_subdirectory("${_ShaderTool_source_dir}" "${ShaderTool_BINARY_DIR}")
    else()
        find_program(ShaderTool_EXECUTABLE
            NAMES ShaderTool
            HINTS "${ShaderTool_ROOT}" "${ShaderTool_ROOT}/bin")
        if(ShaderTool_EXECUTABLE)
            add_executable(ShaderTool IMPORTED GLOBAL)
            set_property(TARGET ShaderTool PROPERTY
                IMPORTED_LOCATION "${ShaderTool_EXECUTABLE}")
        endif()
    endif()
    unset(_ShaderTool_source_dir)
endif()

if(TARGET ShaderTool)
    set(ShaderTool_TARGET ShaderTool)
    if(NOT ShaderTool_EXECUTABLE)
        set(ShaderTool_EXECUTABLE "$<TARGET_FILE:ShaderTool>")
    endif()
    if(ShaderTool_CROSSCOMPILING_EMULATOR)
        set_property(TARGET ShaderTool PROPERTY CROSSCOMPILING_EMULATOR
            "${ShaderTool_CROSSCOMPILING_EMULATOR}")
    endif()
endif()

find_package_handle_standard_args(ShaderTool
    REQUIRED_VARS ShaderTool_TARGET ShaderTool_EXECUTABLE)

if(ShaderTool_FOUND)
    include("${CMAKE_CURRENT_LIST_DIR}/ShaderToolFunctions.cmake")
endif()

mark_as_advanced(ShaderTool_EXECUTABLE ShaderTool_BINARY_DIR)
