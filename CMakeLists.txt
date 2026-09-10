cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ShaderToolStandalone C)
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(ShaderToolSources
    ShaderTool.c
    ShaderToolUtils.c)

add_executable(ShaderTool ${ShaderToolSources})
add_executable(ShaderTool::ShaderTool ALIAS ShaderTool)
target_compile_features(ShaderTool PRIVATE c_std_11)
if(WIN32)
    target_compile_definitions(ShaderTool PRIVATE WIN32_LEAN_AND_MEAN UNICODE)
    set_target_properties(ShaderTool PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(ShaderTool_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ShaderTool")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfig.cmake"
    INSTALL_DESTINATION "${ShaderTool_CMAKE_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolFunctions.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolFunctions.cmake"
    COPYONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolCompilerDiscovery.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolCompilerDiscovery.cmake"
    COPYONLY)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfigVersion.cmake"
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)

install(TARGETS ShaderTool EXPORT ShaderToolTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
# Make config-mode discovery work directly from the build tree as well as from
# an installed prefix. This is useful to parent projects that vendor ShaderTool.
export(EXPORT ShaderToolTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolTargets.cmake"
    NAMESPACE ShaderTool::)
install(EXPORT ShaderToolTargets
    FILE ShaderToolTargets.cmake
    NAMESPACE ShaderTool::
    DESTINATION "${ShaderTool_CMAKE_DIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ShaderToolConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolFunctions.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderToolCompilerDiscovery.cmake"
    DESTINATION "${ShaderTool_CMAKE_DIR}")
