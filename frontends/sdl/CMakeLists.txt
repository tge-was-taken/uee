# What the SDL frontends share: sound, a gamepad and finding a disc. The runtime
# reaches these only through the interfaces a frontend hands it.
find_package(SDL3 3.2 QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "uee_sdl skipped: no SDL3")
    return()
endif()

add_library(uee_sdl STATIC sdl_audio.cpp sdl_disc.cpp sdl_gamepad.cpp)
target_include_directories(uee_sdl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uee_sdl PUBLIC uee SDL3::SDL3 PRIVATE spdlog::spdlog)
