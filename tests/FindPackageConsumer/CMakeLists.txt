cmake_minimum_required(VERSION 3.25)
project(ShaderToolFindPackageConsumer C)

if(NOT SHADER_TOOL_SOURCE_DIR)
    message(FATAL_ERROR "SHADER_TOOL_SOURCE_DIR is required")
endif()

list(PREPEND CMAKE_MODULE_PATH "${SHADER_TOOL_SOURCE_DIR}/cmake")
set(ShaderTool_ROOT "${SHADER_TOOL_SOURCE_DIR}")
find_package(ShaderTool REQUIRED)

if(NOT TARGET ShaderTool)
    message(FATAL_ERROR "FindShaderTool did not provide the ShaderTool target")
endif()
if(NOT COMMAND shadertool_add_shader_objects)
    message(FATAL_ERROR
        "FindShaderTool did not provide shadertool_add_shader_objects")
endif()
