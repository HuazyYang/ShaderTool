cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ShaderToolStandalone C)
    include(CTest)
    set(_shadertool_test_default "${BUILD_TESTING}")
else()
    set(_shadertool_test_default OFF)
endif()

option(SHADERTOOL_BUILD_TESTING "Build ShaderTool's standalone tests"
    "${_shadertool_test_default}")
unset(_shadertool_test_default)

set(ShaderToolSources
    ShaderTool.c
    ShaderToolUtils.c)

add_executable(ShaderTool ${ShaderToolSources})
target_compile_features(ShaderTool PRIVATE c_std_11)
if(WIN32)
    target_compile_definitions(ShaderTool PRIVATE WIN32_LEAN_AND_MEAN UNICODE)
    set_target_properties(ShaderTool PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

if(SHADERTOOL_BUILD_TESTING)
    enable_testing()
    add_executable(ShaderToolFakeCompiler tests/FakeCompiler.c)
    add_executable(ShaderToolUnitTests
        tests/UnitTests.c
        ShaderToolUtils.c)
    target_compile_features(ShaderToolUnitTests PRIVATE c_std_11)
    if(NOT WIN32)
        add_executable(ShaderToolAllBackendsTest ${ShaderToolSources})
        target_compile_features(ShaderToolAllBackendsTest PRIVATE c_std_11)
        target_compile_definitions(ShaderToolAllBackendsTest PRIVATE
            SHADERTOOL_ALLOW_FXC)
    else()
        add_executable(ShaderToolAllBackendsTest ALIAS ShaderTool)
    endif()
    add_test(NAME ShaderToolHelp COMMAND ShaderTool --help)
    set_tests_properties(ShaderToolHelp PROPERTIES PASS_REGULAR_EXPRESSION "fxc\\|dxc\\|spirv")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/unit-tests")
    add_test(NAME ShaderToolUnitTests
        COMMAND ShaderToolUnitTests "${CMAKE_CURRENT_BINARY_DIR}/unit-tests")
    add_test(NAME ShaderToolIntegration
        COMMAND "${CMAKE_COMMAND}"
            "-DSHADER_TOOL=$<TARGET_FILE:ShaderTool>"
            "-DFAKE_COMPILER=$<TARGET_FILE:ShaderToolFakeCompiler>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/integration"
            -P "${CMAKE_CURRENT_LIST_DIR}/tests/Integration.cmake")
    add_test(NAME ShaderToolTimeout
        COMMAND "${CMAKE_COMMAND}"
            "-DSHADER_TOOL=$<TARGET_FILE:ShaderTool>"
            "-DFAKE_COMPILER=$<TARGET_FILE:ShaderToolFakeCompiler>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/timeout"
            -P "${CMAKE_CURRENT_LIST_DIR}/tests/Timeout.cmake")
    add_test(NAME ShaderToolParser
        COMMAND "${CMAKE_COMMAND}"
            "-DSHADER_TOOL=$<TARGET_FILE:ShaderTool>"
            "-DALL_BACKENDS_TOOL=$<TARGET_FILE:ShaderToolAllBackendsTest>"
            "-DFAKE_COMPILER=$<TARGET_FILE:ShaderToolFakeCompiler>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/parser"
            -P "${CMAKE_CURRENT_LIST_DIR}/tests/Parser.cmake")
endif()
