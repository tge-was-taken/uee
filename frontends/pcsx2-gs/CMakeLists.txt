# PCSX2's GS is loaded at run time rather than linked, since it is under PCSX2's
# licence; UEE_PCSX2HOOK names the file.
find_package(SDL3 3.2 QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "pcsx2-gs target skipped: no SDL3")
    return()
endif()

add_executable(pcsx2-gs main.cpp)

# The translated game, when the benchmark targets built one.
if(TARGET translated_p3fes)
    translated_unit_names(P3FES_UNITS ${TRANSLATED_DIR}/p3fes ${BENCH_UNITS})
    target_sources(pcsx2-gs PRIVATE ${P3FES_UNITS})
    target_include_directories(pcsx2-gs PRIVATE ${TRANSLATED_DIR})
    target_compile_definitions(pcsx2-gs PRIVATE UEE_TRANSLATED_P3FES)
    # The rules that write these sources live in the top-level directory, and a
    # custom command only orders the directory it was declared in.
    add_dependencies(pcsx2-gs translated_p3fes)

    get_property(iop_sources GLOBAL PROPERTY UEE_IOP_MODULE_SOURCES)
    if(iop_sources)
        get_property(iop_declarations GLOBAL PROPERTY UEE_IOP_MODULE_DECLARATIONS)
        get_property(iop_list GLOBAL PROPERTY UEE_IOP_MODULE_LIST)
        target_sources(pcsx2-gs PRIVATE ${iop_sources})
        target_compile_definitions(pcsx2-gs PRIVATE
            UEE_IOP_MODULES
            "UEE_IOP_MODULE_DECLARATIONS=${iop_declarations}"
            "UEE_IOP_MODULE_LIST=${iop_list}")
    endif()

    # Translated functions live in this executable, so a discarded packet can
    # only name the guest code that sent it if their symbols are exported.
    set_target_properties(pcsx2-gs PROPERTIES ENABLE_EXPORTS ON)
endif()

target_link_libraries(pcsx2-gs PRIVATE uee spdlog::spdlog SDL3::SDL3 ${CMAKE_DL_LIBS})
