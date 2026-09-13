cmake_minimum_required(VERSION 3.25)
project(uee CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus)

FetchContent_Declare(minicoro
    GIT_REPOSITORY https://github.com/edubart/minicoro.git
    GIT_TAG 02dad0f8b7cbb12fe6e216ae7a76db15ca55cd7b
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(minicoro)
add_library(minicoro STATIC src/minicoro.cpp)
target_include_directories(minicoro PUBLIC ${minicoro_SOURCE_DIR})

set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
FetchContent_Declare(xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.3
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cmake_unofficial)
FetchContent_MakeAvailable(xxhash)

add_library(uee SHARED
    src/opcodes.cpp
    src/instruction.cpp
    src/block_values.cpp
    src/code_map.cpp
    src/translator.cpp
    src/ee_translate.cpp
    src/interpreter.cpp
    src/vu.cpp
    src/vu0_interpreter.cpp
    src/fpu_interpreter.cpp
    src/memory.cpp
    src/guest_replacement.cpp
    src/elf.cpp
    src/kernel.cpp
    src/thread.cpp
    src/semaphores.cpp
    src/timers.cpp
    src/alarm.cpp
    src/ee_mmio.cpp
    src/interrupts.cpp
    src/syscall_table.cpp
    src/sbus.cpp
    src/dmac.cpp
    src/ipu.cpp
    src/movie.cpp
    src/sif.cpp
    src/iop_hle_fileio.cpp
    src/iop_hle_loadfile.cpp
    src/iop_hle_cdvdfsv.cpp
    src/disc.cpp
    src/patches.cpp
    src/vif.cpp
    src/gif.cpp
    src/gs_capture.cpp
    src/gs.cpp
    src/vu_micro.cpp
    src/vu_micro_ops.cpp
    src/vu_capture.cpp
    src/vu_programs.cpp
    src/vu_translate.cpp
    src/irx.cpp
    src/iop.cpp
    src/log.cpp
    src/symbols.cpp
    src/symbol_watch.cpp
    src/idle.cpp
    src/iop_sifcmd.cpp
    src/adpcm.cpp
    src/iop_mmio.cpp
    src/card.cpp
    src/spu2.cpp
    src/pad.cpp
    src/sio2.cpp
    src/iop_interpreter.cpp
    src/iop_kernel.cpp
    src/iop_translate.cpp
    src/iop_run.cpp
    src/run.cpp)
target_include_directories(uee PUBLIC src)
target_link_libraries(uee PUBLIC minicoro tomlplusplus::tomlplusplus spdlog::spdlog
                              xxHash::xxhash)
target_compile_options(uee PRIVATE -fno-semantic-interposition)

# Needs AVX2 for the per-lane shift, and puts the host into a rounding mode the
# whole process then runs in.
option(UEE_HOST_FPU "Use the host float unit where it matches the PS2 unit" OFF)
if(UEE_HOST_FPU)
    target_compile_definitions(uee PUBLIC UEE_HOST_FPU)
    target_compile_options(uee PUBLIC -mavx2)
endif()

enable_testing()

add_executable(run_guest tests/run_guest.cpp)
target_link_libraries(run_guest PRIVATE uee spdlog::spdlog)

add_executable(test_kernel tests/test_kernel.cpp)
target_link_libraries(test_kernel PRIVATE uee spdlog::spdlog)
add_test(NAME kernel_primitives COMMAND test_kernel)

add_executable(test_opcodes tests/test_opcodes.cpp)
target_link_libraries(test_opcodes PRIVATE uee spdlog::spdlog)
add_test(NAME opcodes
         COMMAND test_opcodes ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/decode.tsv)

add_executable(test_execute tests/test_execute.cpp)
target_link_libraries(test_execute PRIVATE uee spdlog::spdlog)
add_test(NAME execute
         COMMAND test_execute ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/execute.tsv)

# Vectors PCSX2's interpreters recorded while games ran. They are too large to
# keep here, so HW_VECTORS_DIR names the directory pcsx2-hook merged them into.
set(HW_VECTORS_DIR "" CACHE PATH "Directory holding the merged *_vectors.jsonl files")
add_executable(test_vu_vectors tests/test_vu_vectors.cpp)
target_link_libraries(test_vu_vectors PRIVATE uee spdlog::spdlog)
add_executable(test_vu_runs tests/test_vu_runs.cpp)
target_link_libraries(test_vu_runs PRIVATE uee spdlog::spdlog)
add_executable(test_vif_vectors tests/test_vif_vectors.cpp)
target_link_libraries(test_vif_vectors PRIVATE uee spdlog::spdlog)
add_executable(test_spu_vectors tests/test_spu_vectors.cpp)
target_link_libraries(test_spu_vectors PRIVATE uee spdlog::spdlog)
if(HW_VECTORS_DIR)
    add_test(NAME vectors.vu0 COMMAND test_vu_vectors ${HW_VECTORS_DIR}/vu0_vectors.jsonl 0)
    add_test(NAME vectors.vu1 COMMAND test_vu_vectors ${HW_VECTORS_DIR}/vu1_vectors.jsonl 1)
    add_test(NAME vectors.vif COMMAND test_vif_vectors ${HW_VECTORS_DIR}/vif_vectors.jsonl)
    add_test(NAME vectors.spu COMMAND test_spu_vectors ${HW_VECTORS_DIR})
endif()

add_executable(test_autotest tests/test_autotest.cpp)
target_link_libraries(test_autotest PRIVATE uee spdlog::spdlog)

add_executable(translate_elf tests/translate_elf.cpp)
target_link_libraries(translate_elf PRIVATE uee spdlog::spdlog)

add_executable(test_iop_autotest tests/test_iop_autotest.cpp)
target_link_libraries(test_iop_autotest PRIVATE uee spdlog::spdlog)

add_executable(translate_irx tests/translate_irx.cpp)
target_link_libraries(translate_irx PRIVATE uee spdlog::spdlog)

add_executable(translate_vu tests/translate_vu.cpp)
target_link_libraries(translate_vu PRIVATE uee spdlog::spdlog)

add_executable(test_boot tests/test_boot.cpp)
target_link_libraries(test_boot PRIVATE uee spdlog::spdlog)

add_executable(test_replacements tests/test_replacements.cpp)
target_link_libraries(test_replacements PRIVATE uee spdlog::spdlog)

# 3dpinball-ps2 md5 da8a5733aa90bd23d15ee3222d1548ca, built from
# https://github.com/headshot2017/3dpinball-ps2. pb::frame is the update loop,
# so entering it repeatedly is what says the game is running.
set(PINBALL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/homebrew/3dpinball-ps2)
# Not in the repository, and without it the game waits on files it cannot open.
set(PINBALL_DATA ${CMAKE_CURRENT_SOURCE_DIR}/scratch/homebrew/3dpinball-ps2/data)
if(EXISTS ${PINBALL_DATA})
    add_test(NAME homebrew.3dpinball
             COMMAND test_boot ${PINBALL_DIR}/3dpinball-ps2.elf --files ${PINBALL_DATA}
                     --enter _ZN2pb5frameEf 60)
else()
    message(STATUS "3dpinball data not found; its boot test is skipped")
endif()

# One unit per function costs about thirty times the CPU and four times the
# machine code, since a compiler folds identical functions only within a unit.
# Two keeps the cross-unit path under test.
set(TRANSLATED_UNITS 2 CACHE STRING "Source files each translated program is split into")

if(NOT PS2SDK AND EXISTS "$ENV{PS2SDK}/iop/startup/linkfile")
    set(PS2SDK "$ENV{PS2SDK}" CACHE PATH "ps2sdk the guest tests build against")
endif()
find_program(EE_GCC mips64r5900el-ps2-elf-gcc
             HINTS $ENV{PS2DEV}/ee/bin /usr/local/ps2dev/ee/bin)

get_filename_component(EE_BIN ${EE_GCC} DIRECTORY)
set(EE_NM ${EE_BIN}/mips64r5900el-ps2-elf-nm)

set(TRANSLATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/translated)
set(GUEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/guest)

function(translated_unit_names out stem count)
    set(names)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        list(APPEND names ${stem}.${i}.cpp)
    endforeach()
    set(${out} ${names} PARENT_SCOPE)
endfunction()

option(UEE_BENCH "Build the translated benchmarks" OFF)
if(UEE_BENCH)
    set(BENCH_UNITS 16)
    # The patches go in because the translator has to read the code that runs.
    function(add_bench name elf)
        set(stem ${TRANSLATED_DIR}/${name})
        set(patches ${CMAKE_CURRENT_SOURCE_DIR}/tests/patches/${name}.toml)
        if(NOT EXISTS ${patches})
            set(patches)
        endif()
        # A frontend reimplements some of the game's functions, and the ones it
        # may take over have to be known here: the check costs a branch, so only
        # these get one. Read from the frontend's own file, not a copy.
        set(addresses ${ARGV2})
        if(addresses AND NOT patches)
            message(FATAL_ERROR "add_bench(${name}): addresses need patches before them")
        endif()
        translated_unit_names(generated ${stem} ${BENCH_UNITS})
        add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                           COMMAND ${CMAKE_COMMAND} -E make_directory ${TRANSLATED_DIR}
                           COMMAND translate_elf ${elf} ${stem} ${BENCH_UNITS} ${patches}
                                   ${addresses}
                           DEPENDS translate_elf ${elf} ${patches} ${addresses}
                           COMMENT "Translating ${name}")
        add_executable(translated_${name} tests/bench_translated.cpp ${generated})
        target_include_directories(translated_${name} PRIVATE ${TRANSLATED_DIR})
        target_link_libraries(translated_${name} PRIVATE uee spdlog::spdlog)
    endfunction()

    function(add_bench_guest name)
        set(elf ${GUEST_DIR}/${name}.elf)
        add_custom_command(OUTPUT ${elf}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GUEST_DIR}
            COMMAND ${EE_GCC} -D_EE -O2 -G0 -Wall
                    -I${PS2SDK}/ee/include -I${PS2SDK}/common/include
                    -L${PS2SDK}/ee/lib -T${PS2SDK}/ee/startup/linkfile
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/${name}.c
                    -o ${elf} -lkernel -lc -lkernel
            COMMAND ${CMAKE_COMMAND} -DNM=${EE_NM} -DELF=${elf} -DOUT=${elf}.map
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteSymbolMap.cmake
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/${name}.c
            COMMENT "Building guest benchmark ${name}")
        add_custom_target(${name}_elf ALL DEPENDS ${elf})
        add_bench(${name} ${elf})
        add_dependencies(translated_${name} ${name}_elf)
    endfunction()

    add_bench(pinball ${PINBALL_DIR}/3dpinball-ps2.elf)

    # The IOP modules a program loads are known before it runs, so they are
    # translated too. Each is translated at the address the loader puts it at,
    # which Iop checks against the image when it loads.
    function(add_bench_iop_module target name irx base loadname)
        set(stem ${TRANSLATED_DIR}/iop.${name})
        translated_unit_names(generated ${stem} 2)
        add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                           COMMAND ${CMAKE_COMMAND} -E make_directory ${TRANSLATED_DIR}
                           COMMAND translate_irx ${irx} ${stem} ${name} 2 ${base} ${loadname}
                           DEPENDS translate_irx ${irx}
                           COMMENT "Translating IOP module ${name}")
        target_sources(${target} PRIVATE ${generated})
    endfunction()

    # Every IOP module on the disc, each owning a fixed slot so the translated
    # code needs no relocation. Anything loaded that is not in here lands above
    # the whole range, which is why translating all of them is safer than
    # picking a few. Regenerate with the p3fes_modules target when the disc or
    # the host list changes.
    function(add_disc_iop_modules target iso config manifest)
        if(NOT EXISTS ${manifest})
            message(STATUS "no IOP module manifest at ${manifest}")
            return()
        endif()
        add_custom_target(${target}_modules
                          COMMAND translate_irx --list ${iso} ${config} > ${manifest}
                          DEPENDS translate_irx
                          COMMENT "Listing IOP modules on the disc")

        set(declarations)
        set(list)
        set(sources)
        file(STRINGS ${manifest} lines)
        foreach(line ${lines})
            if(NOT line MATCHES "^([^ ]+) 0x[0-9a-f]+ 0x[0-9a-f]+$")
                continue()
            endif()
            set(module ${CMAKE_MATCH_1})
            string(REGEX REPLACE "\\..*$" "" symbol ${module})
            set(stem ${TRANSLATED_DIR}/iop.${symbol})
            translated_unit_names(generated ${stem} 2)
            add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                               COMMAND ${CMAKE_COMMAND} -E make_directory ${TRANSLATED_DIR}
                               COMMAND translate_irx --module ${module} ${iso} ${config}
                                       ${stem} 2
                               DEPENDS translate_irx ${iso} ${config} ${manifest}
                               COMMENT "Translating IOP module ${module}")
            target_sources(${target} PRIVATE ${generated})
            list(APPEND sources ${generated})
            string(APPEND declarations "extern TranslatedIrx ${symbol}Module\; ")
            list(APPEND list "&uee::${symbol}Module")
        endforeach()
        list(JOIN list ", " list)
        target_compile_definitions(${target} PRIVATE
            UEE_IOP_MODULES
            "UEE_IOP_MODULE_DECLARATIONS=${declarations}"
            "UEE_IOP_MODULE_LIST=${list}")
        # The translation is generated once; any other target that runs the same
        # game links the same files rather than translating them again.
        set_property(GLOBAL PROPERTY UEE_IOP_MODULE_SOURCES "${sources}")
        set_property(GLOBAL PROPERTY UEE_IOP_MODULE_DECLARATIONS "${declarations}")
        set_property(GLOBAL PROPERTY UEE_IOP_MODULE_LIST "${list}")
    endfunction()

    # The micro programs a run recorded with UEE_VU_MANIFEST. Nothing on the disc
    # says which programs a game uploads, so the game has to have run them.
    function(add_vu_programs name manifest)
        if(NOT EXISTS ${manifest})
            message(STATUS "no VU program manifest at ${manifest}")
            return()
        endif()
        set(stem ${TRANSLATED_DIR}/vu.${name})
        translated_unit_names(generated ${stem} 4)
        add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                           COMMAND ${CMAKE_COMMAND} -E make_directory ${TRANSLATED_DIR}
                           COMMAND translate_vu ${manifest} ${stem} 4
                           DEPENDS translate_vu ${manifest}
                           COMMENT "Translating ${name}'s VU programs")
        # A custom command only orders its own directory, so a target in another
        # one depends on this instead.
        add_custom_target(${name}_vu_programs DEPENDS ${generated})
        # Replays a UEE_VU_CAPTURE of this game against the same translations.
        add_executable(bench_vu_${name} tests/bench_vu.cpp ${generated})
        target_include_directories(bench_vu_${name} PRIVATE ${TRANSLATED_DIR})
        target_compile_definitions(bench_vu_${name} PRIVATE UEE_VU_PROGRAMS)
        target_link_libraries(bench_vu_${name} PRIVATE uee spdlog::spdlog)
        set_property(GLOBAL PROPERTY UEE_VU_PROGRAM_SOURCES "${generated}")
        set_property(GLOBAL PROPERTY UEE_VU_PROGRAM_TARGET ${name}_vu_programs)
    endfunction()

    # A stripped retail executable, where discovery has only the code to go on.
    set(P3FES_ELF ${CMAKE_CURRENT_SOURCE_DIR}/scratch/games/p3fes/SLUS_216.21)
    if(EXISTS ${P3FES_ELF})
        add_test(NAME replacements.p3fes COMMAND test_replacements ${P3FES_ELF})
        if(DEFINED ENV{SKY2HOOK_DIR})
            set(P3FES_ADDRESSES "$ENV{SKY2HOOK_DIR}/config/SLUS-21621.toml")
        endif()
        if(NOT EXISTS "${P3FES_ADDRESSES}")
            set(P3FES_ADDRESSES)
        endif()
        add_bench(p3fes ${P3FES_ELF} ${P3FES_ADDRESSES})
        add_disc_iop_modules(translated_p3fes
                             ${CMAKE_CURRENT_SOURCE_DIR}/scratch/games/p3fes/p3fes.iso
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/patches/p3fes.toml
                             ${CMAKE_CURRENT_SOURCE_DIR}/scratch/games/p3fes/modules.txt)
        add_vu_programs(p3fes ${CMAKE_CURRENT_SOURCE_DIR}/scratch/games/p3fes/vu_programs.txt)
    endif()

    if(EE_GCC AND EXISTS "${PS2SDK}/ee/startup/linkfile")
        add_bench_guest(arithmetic)
        add_bench_guest(shapes)
        target_compile_options(translated_shapes PRIVATE -g)
    endif()

endif()

add_subdirectory(tools/gsdump)

set(AUTOTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ps2autotests/tests)
# Every vendored ELF with recorded output, so none is left unrun by omission.
file(GLOB_RECURSE AUTOTEST_CANDIDATES ${AUTOTEST_DIR}/*.elf)
set(AUTOTEST_ELFS)
foreach(elf ${AUTOTEST_CANDIDATES})
    string(REGEX REPLACE "\\.elf$" ".expected" expected ${elf})
    if(EXISTS ${expected})
        list(APPEND AUTOTEST_ELFS ${elf})
    endif()
endforeach()
foreach(elf ${AUTOTEST_ELFS})
    file(RELATIVE_PATH relative ${AUTOTEST_DIR} ${elf})
    string(REGEX REPLACE "\\.elf$" "" relative ${relative})
    string(REPLACE "/" "." name ${relative})
    add_test(NAME ${name} COMMAND test_autotest ${elf} ${AUTOTEST_DIR}/${relative}.expected)
endforeach()


foreach(elf ${AUTOTEST_ELFS})
    file(RELATIVE_PATH relative ${AUTOTEST_DIR} ${elf})
    string(REGEX REPLACE "\\.elf$" "" relative ${relative})
    string(REPLACE "/" "." name ${relative})
    set(stem ${TRANSLATED_DIR}/${name})
    translated_unit_names(generated ${stem} ${TRANSLATED_UNITS})
    add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                       COMMAND ${CMAKE_COMMAND} -E make_directory
                               ${TRANSLATED_DIR}
                       COMMAND translate_elf ${elf} ${stem} ${TRANSLATED_UNITS}
                       DEPENDS translate_elf ${elf}
                       COMMENT "Translating ${name}")
    add_executable(translated_${name} tests/test_translated.cpp ${generated})
    target_include_directories(translated_${name} PRIVATE
                               ${TRANSLATED_DIR})
    target_link_libraries(translated_${name} PRIVATE uee spdlog::spdlog)
    add_test(NAME translated.${name}
             COMMAND translated_${name} ${elf} ${AUTOTEST_DIR}/${relative}.expected)
endforeach()

# Known failures. They still run, and a fixed one turns the suite red.
set(KNOWN_FAILURES)
foreach(name ${KNOWN_FAILURES})
    set_tests_properties(${name} translated.${name} PROPERTIES WILL_FAIL TRUE)
endforeach()

file(GLOB IOP_AUTOTEST_IRXS ${AUTOTEST_DIR}/cpu/iop/*.irx)
foreach(irx ${IOP_AUTOTEST_IRXS})
    file(RELATIVE_PATH relative ${AUTOTEST_DIR} ${irx})
    string(REGEX REPLACE "\\.irx$" "" relative ${relative})
    string(REPLACE "/" "." name ${relative})

    # A module with nothing recorded to compare against is not a test yet.
    if(NOT EXISTS ${AUTOTEST_DIR}/${relative}.expected)
        continue()
    endif()

    add_test(NAME ${name} COMMAND test_iop_autotest ${irx} ${AUTOTEST_DIR}/${relative}.expected)

    # The module is named after itself in the generated source, so the name has
    # to be spellable as a C++ identifier.
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" TRANSLATED_MODULE ${name})
    set(stem ${TRANSLATED_DIR}/${name})
    translated_unit_names(generated ${stem} ${TRANSLATED_UNITS})
    add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                       COMMAND ${CMAKE_COMMAND} -E make_directory
                               ${TRANSLATED_DIR}
                       COMMAND translate_irx ${irx} ${stem} ${TRANSLATED_MODULE}
                               ${TRANSLATED_UNITS}
                       DEPENDS translate_irx ${irx}
                       COMMENT "Translating ${name}")
    set(driver ${TRANSLATED_DIR}/${name}.main.cpp)
    configure_file(tests/test_iop_translated.cpp.in ${driver} @ONLY)
    add_executable(translated_${name} ${driver} ${generated})
    target_include_directories(translated_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
                               ${TRANSLATED_DIR})
    target_link_libraries(translated_${name} PRIVATE uee spdlog::spdlog)
    add_test(NAME translated.${name}
             COMMAND translated_${name} ${irx} ${AUTOTEST_DIR}/${relative}.expected)
endforeach()

# Expected output derived from the resident IOP services in the module IDBs.
if(IOP_GCC AND EXISTS "${PS2SDK}/iop/startup/linkfile")
    set(IOP_GUEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/iop_guest)
    set(IOP_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/iop_guest)
    # -mno-explicit-relocs keeps each high half next to its own low half, which
    # is the only pairing the IRX loader relocates.
    set(IOP_CFLAGS -D_IOP -fno-builtin -G0 -Os -Wall -msoft-float -mno-explicit-relocs
                   -I${PS2SDK}/iop/include -I${PS2SDK}/common/include)
    foreach(guest printf sysclib thsemap thevent thbase sysmem intrman sio2)
        set(irx ${IOP_BUILD_DIR}/${guest}.irx)
        add_custom_command(OUTPUT ${irx}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${IOP_BUILD_DIR}
            COMMAND ${IOP_GCC} ${IOP_CFLAGS} -fno-toplevel-reorder
                    -c ${IOP_GUEST_DIR}/imports.c -o ${IOP_BUILD_DIR}/${guest}.imports.o
            COMMAND ${IOP_GCC} ${IOP_CFLAGS}
                    -c ${IOP_GUEST_DIR}/${guest}.c -o ${IOP_BUILD_DIR}/${guest}.o
            COMMAND ${IOP_GCC} ${IOP_CFLAGS} -T${PS2SDK}/iop/startup/linkfile -Os -nostdlib -dc -r
                    -o ${IOP_BUILD_DIR}/${guest}.elf
                    ${IOP_BUILD_DIR}/${guest}.o ${IOP_BUILD_DIR}/${guest}.imports.o
            COMMAND ${PS2SDK}/bin/iopfixup --rb --irx1 --allow-zero-text
                    -o ${irx} ${IOP_BUILD_DIR}/${guest}.elf
            DEPENDS ${IOP_GUEST_DIR}/${guest}.c ${IOP_GUEST_DIR}/imports.c
            COMMENT "Building IOP guest test ${guest}")
        add_custom_target(iop_guest_${guest} ALL DEPENDS ${irx})
        add_test(NAME iop_guest.${guest}
                 COMMAND test_iop_autotest ${irx} ${IOP_GUEST_DIR}/${guest}.expected)
    endforeach()
else()
    message(STATUS "IOP guest tests skipped: IOP_GCC=${IOP_GCC} PS2SDK=${PS2SDK}")
endif()

# Expected output derived from the kernel behaviour in the IDBs and, for the
# hardware paths, from the unit manuals, not from hardware itself.
if(EE_GCC AND EXISTS "${PS2SDK}/ee/startup/linkfile")
    foreach(guest alarm thread_sema coverage degrade
                  delay priority thread_state sema intc sif_regs syscall_hook machine resume
                  recursion path1 draw)
        set(elf ${GUEST_DIR}/${guest}.elf)
        add_custom_command(OUTPUT ${elf}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GUEST_DIR}
            COMMAND ${EE_GCC} -D_EE -O2 -G0 -Wall
                    -I${PS2SDK}/ee/include -I${PS2SDK}/common/include
                    -L${PS2SDK}/ee/lib -T${PS2SDK}/ee/startup/linkfile
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/${guest}.c
                    -o ${elf} -lkernel -lc -lkernel
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/${guest}.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/syscall.h
            COMMENT "Building guest test ${guest}")
        add_custom_target(guest_${guest} ALL DEPENDS ${elf})
        add_test(NAME guest.${guest}
                 COMMAND test_autotest ${elf}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/${guest}.expected)

        # The same program through the translator. A guest test blocks and
        # switches threads where the vendored autotests do not, which is the
        # only way the translated path is asked what a resumed function is.
        set(stem ${TRANSLATED_DIR}/guest.${guest})
        translated_unit_names(generated ${stem} ${TRANSLATED_UNITS})
        add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                           COMMAND ${CMAKE_COMMAND} -E make_directory ${TRANSLATED_DIR}
                           COMMAND translate_elf ${elf} ${stem} ${TRANSLATED_UNITS}
                           DEPENDS translate_elf ${elf}
                           COMMENT "Translating guest ${guest}")
        add_executable(translated_guest_${guest} tests/test_translated.cpp ${generated})
        target_include_directories(translated_guest_${guest} PRIVATE ${TRANSLATED_DIR})
        target_link_libraries(translated_guest_${guest} PRIVATE uee spdlog::spdlog)
        add_test(NAME translated.guest.${guest}
                 COMMAND translated_guest_${guest} ${elf}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/${guest}.expected)
    endforeach()
else()
    message(STATUS "guest tests skipped: EE_GCC=${EE_GCC} PS2SDK=${PS2SDK}")
endif()

add_subdirectory(frontends/sky2hook)
add_subdirectory(frontends/pcsx2-gs)
