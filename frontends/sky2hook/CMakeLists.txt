# The P3FES PC port's renderer on this runtime, compiled from the working copy
# SKY2HOOK_DIR names, since the two trees move together.
set(SKY2HOOK_DIR "" CACHE PATH "P3FES PC port working copy the renderer is compiled from")
if(DEFINED ENV{SKY2HOOK_DIR})
    set(SKY2HOOK_DIR "$ENV{SKY2HOOK_DIR}")
endif()

if(NOT SKY2HOOK_DIR)
    message(STATUS "p3fes target skipped: SKY2HOOK_DIR names no P3FES PC port")
    return()
endif()
if(NOT EXISTS "${SKY2HOOK_DIR}/src/runtime/renderware/sky2/skygl.h")
    message(FATAL_ERROR "SKY2HOOK_DIR=${SKY2HOOK_DIR} is not a P3FES PC port working copy")
endif()

enable_language(C)  # the GL loader the port carries is C

# The port carries its own warning policy; this build's does not apply to it.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

find_package(SDL3 3.2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)

set(PORT "${SKY2HOOK_DIR}/src")
set(SKY "${PORT}/runtime/renderware/sky2")

FetchContent_Declare(pl_mpeg
    GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg.git
    GIT_TAG c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(pl_mpeg)
add_library(pl_mpeg STATIC pl_mpeg.cpp)
target_include_directories(pl_mpeg PUBLIC ${pl_mpeg_SOURCE_DIR})
target_compile_options(pl_mpeg PRIVATE -w)

add_executable(p3fes
    uee_hooks.cpp
    uee_runtime.cpp
    movie_decoder.cpp
    "${PORT}/main.cpp"
    "${PORT}/app/app.cpp"
    "${PORT}/app/config.cpp"
    "${PORT}/core/guest.cpp"
    "${PORT}/game/config.cpp"
    "${PORT}/game/enhancements/interp_tags.cpp"
    "${PORT}/game/enhancements/intro_skip.cpp"
    "${PORT}/game/enhancements/movie_skip.cpp"
    "${PORT}/game/enhancements/speed.cpp"
    "${PORT}/game/trace/symbols.cpp"
    "${PORT}/runtime/hooks.cpp"
    "${PORT}/runtime/trace/draw_context.cpp"
    "${PORT}/runtime/trace/gs_draws.cpp"
    "${PORT}/runtime/trace/recording.cpp"
    "${PORT}/runtime/trace/replay.cpp"
    "${PORT}/runtime/trace/vif_trace.cpp"
    "${SKY}/common/frame.cpp"
    "${SKY}/common/geometry_pool.cpp"
    "${SKY}/enhancements/feedback_blur.cpp"
    "${SKY}/enhancements/interpolator.cpp"
    "${SKY}/enhancements/interpolator_debug.cpp"
    "${SKY}/enhancements/msaa.cpp"
    "${SKY}/enhancements/shadow_detail.cpp"
    "${SKY}/material/material.cpp"
    "${SKY}/material/pipeline_dump.cpp"
    "${SKY}/pipelines/atomic/atomic.cpp"
    "${SKY}/pipelines/atomic/ptank.cpp"
    "${SKY}/pipelines/im2d/im2d.cpp"
    "${SKY}/pipelines/im3d/im3d.cpp"
    "${SKY}/pipelines/rt2d/rt2d.cpp"
    "${SKY}/pipelines/sector/native_data.cpp"
    "${SKY}/pipelines/sector/vif.cpp"
    "${SKY}/pipelines/sector/sector.cpp"
    "${SKY}/skygl_device.cpp"
    "${SKY}/skygl_gl.cpp"
    "${SKY}/skygl_hooks.cpp"
    "${SKY}/skygl_imgui.cpp"
    "${SKY}/skygl_object_manager.cpp"
    "${SKY}/skygl_render_queue.cpp"
    "${SKY}/skygl_renderer.cpp"
    "${SKY}/skygl_rw_to_gl.cpp"
    "${SKY}/skygl_shader_manager.cpp"
    "${SKY}/skygl_texture_decoder.cpp"
    "${SKY}/skygl_texture_manager.cpp"
    "${SKY}/skygl_trace.cpp"
    "${SKY}/skygl_window.cpp"
    "${SKY2HOOK_DIR}/thirdparty/glad/src/gl.c"
    "${SKY2HOOK_DIR}/thirdparty/imgui/src/imgui.cpp"
    "${SKY2HOOK_DIR}/thirdparty/imgui/src/imgui_draw.cpp"
    "${SKY2HOOK_DIR}/thirdparty/imgui/src/imgui_tables.cpp"
    "${SKY2HOOK_DIR}/thirdparty/imgui/src/imgui_widgets.cpp")

# The warnings this build runs with, kept for the two files it owns.
set_source_files_properties(uee_hooks.cpp uee_runtime.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")

# The port is C++20 and this runtime is C++23; each half keeps its own.
set_target_properties(p3fes PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The shaders and per-serial address files are read at runtime from the port.
target_compile_definitions(p3fes PRIVATE
    SKYGL_SHADER_DIR="${SKY}/shaders"
    GAME_CONFIG_DIR="${SKY2HOOK_DIR}/config")

target_include_directories(p3fes PRIVATE
    "${PORT}"
    "${PORT}/app"
    "${PORT}/core"
    "${PORT}/game/trace"
    "${PORT}/runtime"
    "${PORT}/runtime/trace"
    "${SKY}"
    "${SKY}/common"
    "${SKY}/material"
    "${SKY2HOOK_DIR}/thirdparty/glad/include"
    "${SKY2HOOK_DIR}/thirdparty/imgui/include"
    "${SKY2HOOK_DIR}/thirdparty/tomlplusplus")

# The translated game, when the benchmark targets built one.
if(TARGET translated_p3fes)
    translated_unit_names(P3FES_UNITS ${TRANSLATED_DIR}/p3fes ${BENCH_UNITS})
    target_sources(p3fes PRIVATE ${P3FES_UNITS}
                   ${TRANSLATED_DIR}/iop.LIBSD.0.cpp ${TRANSLATED_DIR}/iop.LIBSD.1.cpp
                   ${TRANSLATED_DIR}/iop.CRI_ADXI.0.cpp ${TRANSLATED_DIR}/iop.CRI_ADXI.1.cpp)
    target_include_directories(p3fes PRIVATE ${TRANSLATED_DIR})
    target_compile_definitions(p3fes PRIVATE UEE_TRANSLATED_P3FES)
    # The rules that write these sources live in the top-level directory, and a
    # custom command only orders the directory it was declared in.
    add_dependencies(p3fes translated_p3fes)
endif()

get_property(iop_sources GLOBAL PROPERTY UEE_IOP_MODULE_SOURCES)
if(iop_sources)
    get_property(iop_declarations GLOBAL PROPERTY UEE_IOP_MODULE_DECLARATIONS)
    get_property(iop_list GLOBAL PROPERTY UEE_IOP_MODULE_LIST)
    target_sources(p3fes PRIVATE ${iop_sources})
    target_compile_definitions(p3fes PRIVATE
        UEE_IOP_MODULES
        "UEE_IOP_MODULE_DECLARATIONS=${iop_declarations}"
        "UEE_IOP_MODULE_LIST=${iop_list}")
endif()

# Translated functions live in this executable, so a discarded packet can only
# name the guest code that sent it if their symbols are in the dynamic table.
set_target_properties(p3fes PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(p3fes PRIVATE
    uee
    pl_mpeg
    spdlog::spdlog
    SDL3::SDL3
    ZLIB::ZLIB
    OpenGL::GL
    OpenGL::GLX
    X11::X11
    Threads::Threads)
