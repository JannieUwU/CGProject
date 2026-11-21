cmake_minimum_required(VERSION 3.21)
project(planet_sandbox LANGUAGES C CXX)

option(PLANET_BUILD_DESKTOP "Build the native planet editor" OFF)
if(APPLE AND PLANET_BUILD_DESKTOP)
    # GLFW uses Objective-C; the Metal frontend uses Objective-C++.
    # Enable both before creating targets so .m files are not compiled as C++.
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(REBOUND_SOURCE_DIR "" CACHE PATH "Local REBOUND source checkout")

if(REBOUND_SOURCE_DIR)
    set(rebound_SOURCE_DIR "${REBOUND_SOURCE_DIR}")
else()
    FetchContent_Declare(rebound
        URL https://github.com/hannorein/rebound/archive/refs/tags/5.0.1.tar.gz)
    FetchContent_GetProperties(rebound)
    if(NOT rebound_POPULATED)
        FetchContent_Populate(rebound)
    endif()
endif()

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(nlohmann_json)

file(GLOB REBOUND_SOURCES CONFIGURE_DEPENDS "${rebound_SOURCE_DIR}/src/*.c")
add_library(rebound STATIC ${REBOUND_SOURCES})
target_include_directories(rebound PUBLIC "${rebound_SOURCE_DIR}/src")
target_compile_definitions(rebound PRIVATE _GNU_SOURCE GITHASH=0)
if(APPLE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # Preserve separate rounding in REBOUND's compensated gravity calculation.
    # FMA contraction can stall IAS15 for symmetric initial conditions on macOS.
    target_compile_options(rebound PRIVATE -ffp-contract=off)
endif()
if(WIN32)
    target_compile_definitions(rebound PUBLIC BUILDINGLIBREBOUND)
endif()
set_target_properties(rebound PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX AND NOT APPLE)
    target_link_libraries(rebound PUBLIC m pthread)
endif()

add_library(planet_core
    src/project.cpp
    src/solar_system.cpp
    src/texture.cpp
    src/solar_shape.cpp
    src/planet_visual.cpp
    src/sculpt.cpp
    src/simulation.cpp
    src/demo_presets.cpp
    src/session.cpp
    src/renderer.cpp
    src/raster_renderer.cpp
    src/triangle_coverage.cpp
    src/aa_benchmark.cpp
    src/png_writer.cpp)
target_include_directories(planet_core PUBLIC include)
target_include_directories(planet_core PRIVATE third_party)
target_compile_definitions(planet_core PRIVATE PLANET_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/solar-system")
target_link_libraries(planet_core PUBLIC rebound nlohmann_json::nlohmann_json)

add_executable(planet_sandbox src/main.cpp)
target_link_libraries(planet_sandbox PRIVATE planet_core)

add_executable(planet_aa_benchmark src/aa_benchmark_main.cpp)
target_link_libraries(planet_aa_benchmark PRIVATE planet_core)
if(WIN32)
    target_link_libraries(planet_aa_benchmark PRIVATE shell32)
endif()

enable_testing()
add_executable(planet_core_tests tests/core_tests.cpp)
target_link_libraries(planet_core_tests PRIVATE planet_core)
if(MSVC)
    target_compile_options(planet_core_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(planet_core_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME planet_core_tests COMMAND planet_core_tests)
add_executable(planet_pixel_trace_tests tests/pixel_trace_tests.cpp)
target_link_libraries(planet_pixel_trace_tests PRIVATE planet_core)
if(MSVC)
    target_compile_options(planet_pixel_trace_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(planet_pixel_trace_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME planet_pixel_trace_tests COMMAND planet_pixel_trace_tests)
add_executable(planet_shared_edge_tests tests/shared_edge_tests.cpp)
target_link_libraries(planet_shared_edge_tests PRIVATE planet_core)
if(MSVC)
    target_compile_options(planet_shared_edge_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(planet_shared_edge_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME planet_shared_edge_tests COMMAND planet_shared_edge_tests)
add_executable(planet_msaa_shading_tests tests/msaa_shading_tests.cpp)
target_link_libraries(planet_msaa_shading_tests PRIVATE planet_core)
if(MSVC)
    target_compile_options(planet_msaa_shading_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(planet_msaa_shading_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME planet_msaa_shading_tests COMMAND planet_msaa_shading_tests)
add_executable(planet_visual_tests tests/planet_visual_tests.cpp)
target_link_libraries(planet_visual_tests PRIVATE planet_core)
if(MSVC)
    target_compile_options(planet_visual_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(planet_visual_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME planet_visual_tests COMMAND planet_visual_tests)
add_test(NAME planet_aa_benchmark_smoke
    COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:planet_aa_benchmark>
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/aa_benchmark_smoke.cmake)

if(PLANET_BUILD_DESKTOP)
    find_package(Threads REQUIRED)
    find_package(glfw3 3.3 QUIET)
    if(NOT TARGET glfw)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz)
        FetchContent_MakeAvailable(glfw)
    endif()

    FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz)
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC glfw)

    add_library(planet_studio_support STATIC
        src/studio_ui.cpp src/studio_pixel_trace.cpp src/studio_workflow.cpp src/studio_measurement.cpp src/studio_render_worker.cpp
        src/project_history.cpp src/project_recovery.cpp src/studio_document.cpp)
    target_link_libraries(planet_studio_support PUBLIC planet_core imgui Threads::Threads)

    if(APPLE)
        set_source_files_properties(src/studio_metal.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
        add_executable(planet_studio src/studio_metal.mm)
        target_link_libraries(planet_studio PRIVATE planet_studio_support
            "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
    else()
        find_package(OpenGL REQUIRED)
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
        add_executable(planet_studio src/studio_gl.cpp)
        target_link_libraries(planet_studio PRIVATE planet_studio_support OpenGL::GL)
    endif()
    add_executable(planet_studio_tests tests/studio_tests.cpp)
    target_link_libraries(planet_studio_tests PRIVATE planet_studio_support)
    if(MSVC)
        target_compile_options(planet_studio_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(planet_studio_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME planet_studio_tests COMMAND planet_studio_tests)
    add_executable(planet_studio_render_tests tests/studio_render_tests.cpp)
    target_link_libraries(planet_studio_render_tests PRIVATE planet_studio_support)
    if(MSVC)
        target_compile_options(planet_studio_render_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(planet_studio_render_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME planet_studio_render_tests COMMAND planet_studio_render_tests)
    add_executable(planet_studio_document_tests tests/studio_document_tests.cpp)
    target_link_libraries(planet_studio_document_tests PRIVATE planet_studio_support)
    if(MSVC)
        target_compile_options(planet_studio_document_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(planet_studio_document_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME planet_studio_document_tests COMMAND planet_studio_document_tests)
endif()
