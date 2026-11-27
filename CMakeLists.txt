# The parts every add-on shares: no ReShade calls, no overlay, no game hooks of its own, so it builds
# and tests on its own. A second add-on links it with
#
#     add_subdirectory(<this directory> core)
#     target_link_libraries(my_addon PRIVATE phantom_fx_core)
#
# See docs/addon-core.md.
cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(phantom_fx_core LANGUAGES CXX)
    enable_testing()
    set(PHANTOM_FX_CORE_TESTS_DEFAULT ON)
else()
    # A consumer does not build another project's tests unless it asks.
    set(PHANTOM_FX_CORE_TESTS_DEFAULT OFF)
endif()
option(PHANTOM_FX_CORE_TESTS "Build phantom_fx_core's tests" ${PHANTOM_FX_CORE_TESTS_DEFAULT})
# Compile an add-on's HLSL at run time, from its checkout, so a shader edit needs no rebuild. Off, the
# add-on ships only the bytecode fxc made at build time and cannot compile a shader at run time. The
# add-on's src/ is the shaders' root, as it is for its includes.
option(PHANTOM_DEV_SHADERS "Compile an add-on's HLSL at run time (development only)" OFF)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "phantom_fx_core requires Windows and MSVC.")
endif()

# game_code.hpp includes MinHook. A consumer that already has it (PhantomFX) skips the download.
if(NOT TARGET minhook)
    include(FetchContent)
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
    )
    FetchContent_MakeAvailable(minhook)
endif()

# The add-on source root: `core/...` resolves from it, and the headers that moved out of the
# add-on keep their names, so `log.hpp` and `game_shaders.hpp` resolve from here too.
set(PHANTOM_FX_CORE_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# `core/...` needs this directory to be named `core` (a consumer mounts it at src/core). Built alone from a checkout
# with another name, a junction named core stands in for that.
get_filename_component(PHANTOM_FX_CORE_DIR_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
if(NOT PHANTOM_FX_CORE_DIR_NAME STREQUAL "core")
    set(PHANTOM_FX_CORE_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/include")
    file(MAKE_DIRECTORY "${PHANTOM_FX_CORE_INCLUDE}")
    if(NOT EXISTS "${PHANTOM_FX_CORE_INCLUDE}/core")
        file(TO_NATIVE_PATH "${PHANTOM_FX_CORE_INCLUDE}/core" link_path)
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" link_target)
        execute_process(COMMAND cmd /c mklink /J "${link_path}" "${link_target}" RESULT_VARIABLE link_result OUTPUT_QUIET)
        if(NOT link_result EQUAL 0)
            message(FATAL_ERROR "Could not link ${link_path} to ${link_target}; check the core out into a directory named core.")
        endif()
    endif()
endif()

add_library(phantom_fx_core STATIC
    order.cpp
    gpu_timer.cpp
    log.cpp
    settings/ini.cpp
    shader_bytecode.cpp
    game/camera.cpp
    game/game_shaders.cpp
    game/device_check.cpp
    capture/constant_capture.cpp
)
target_include_directories(phantom_fx_core PUBLIC
    "${PHANTOM_FX_CORE_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/game"
    "${CMAKE_CURRENT_SOURCE_DIR}/capture"
)
target_compile_features(phantom_fx_core PUBLIC cxx_std_20)
target_compile_definitions(phantom_fx_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(phantom_fx_core PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(phantom_fx_core PUBLIC d3d11 d3dcompiler minhook)
set_target_properties(phantom_fx_core PROPERTIES CXX_EXTENSIONS OFF)
if(PHANTOM_DEV_SHADERS)
    target_sources(phantom_fx_core PRIVATE shader_source.cpp)
    target_compile_definitions(phantom_fx_core PUBLIC PHANTOM_DEV_SHADERS=1
        PHANTOM_DEV_SHADER_ROOT="${PHANTOM_FX_CORE_INCLUDE}")
else()
    target_compile_definitions(phantom_fx_core PUBLIC PHANTOM_DEV_SHADERS=0)
endif()

# An add-on includes this for phantom_precompile_shader and phantom_forbid_runtime_compile. Set in the
# parent scope too: a variable a subdirectory sets does not reach the add-on's CMakeLists.
set(PHANTOM_FX_CORE_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(PHANTOM_FX_CORE_CMAKE_DIR "${PHANTOM_FX_CORE_CMAKE_DIR}" PARENT_SCOPE)

include(CTest)
if(PHANTOM_FX_CORE_TESTS AND BUILD_TESTING)
    add_executable(core_order_tests tests/core_order_tests.cpp)
    target_link_libraries(core_order_tests PRIVATE phantom_fx_core)
    target_compile_options(core_order_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME core_order COMMAND core_order_tests)

    add_executable(core_settings_tests tests/core_settings_tests.cpp)
    target_link_libraries(core_settings_tests PRIVATE phantom_fx_core)
    target_compile_options(core_settings_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME core_settings COMMAND core_settings_tests)

    add_executable(core_gpu_timer_tests tests/core_gpu_timer_tests.cpp)
    target_link_libraries(core_gpu_timer_tests PRIVATE phantom_fx_core)
    target_compile_options(core_gpu_timer_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME core_gpu_timer COMMAND core_gpu_timer_tests)
endif()
