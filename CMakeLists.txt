cmake_minimum_required(VERSION 3.25)
project(uee CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_library(uee
    src/opcodes.cpp
    src/instruction.cpp
    src/translate.cpp
    src/interpreter.cpp
    src/vu.cpp
    src/vu0_interpreter.cpp
    src/fpu_interpreter.cpp
    src/memory.cpp
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
    src/gs.cpp
    src/vu_micro.cpp
    src/irx.cpp
    src/iop.cpp
    src/log.cpp
    src/symbols.cpp
    src/hooks.cpp
    src/idle.cpp
    src/iop_rpc.cpp
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
target_link_libraries(uee PUBLIC minicoro tomlplusplus::tomlplusplus spdlog::spdlog)

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

add_executable(test_autotest tests/test_autotest.cpp)
target_link_libraries(test_autotest PRIVATE uee spdlog::spdlog)

add_executable(translate_elf tests/translate_elf.cpp)
target_link_libraries(translate_elf PRIVATE uee spdlog::spdlog)

add_executable(test_iop_autotest tests/test_iop_autotest.cpp)
target_link_libraries(test_iop_autotest PRIVATE uee spdlog::spdlog)

add_executable(translate_irx tests/translate_irx.cpp)
target_link_libraries(translate_irx PRIVATE uee spdlog::spdlog)

add_executable(test_boot tests/test_boot.cpp)
target_link_libraries(test_boot PRIVATE uee spdlog::spdlog)

# 3dpinball-ps2 md5 da8a5733aa90bd23d15ee3222d1548ca, built from
# https://github.com/headshot2017/3dpinball-ps2. A whole game exercises paths no
# autotest reaches, and pb::frame is the update loop, so entering it repeatedly
# is the only sign the game is running rather than stuck in a splash screen.
set(PINBALL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/homebrew/3dpinball-ps2)
# The game reads its own data off the mount, so without it it waits on files it
# cannot open. The tree is not in the repository, so the test needs it present.
set(PINBALL_DATA ${CMAKE_CURRENT_SOURCE_DIR}/scratch/homebrew/3dpinball-ps2/data)
if(EXISTS ${PINBALL_DATA})
    add_test(NAME homebrew.3dpinball
             COMMAND test_boot ${PINBALL_DIR}/3dpinball-ps2.elf --files ${PINBALL_DATA}
                     --enter _ZN2pb5frameEf 60)
else()
    message(STATUS "3dpinball data not found; its boot test is skipped")
endif()

# A whole game needs the translation split over many units: one unit per function
# costs about thirty times the CPU and four times the machine code, because a
# compiler can only fold identical functions inside a single unit. Two is enough
# here to keep the cross-unit path under test.
set(TRANSLATED_UNITS 2 CACHE STRING "Source files each translated program is split into")

function(translated_unit_names out stem count)
    set(names)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        list(APPEND names ${stem}.${i}.cpp)
    endforeach()
    set(${out} ${names} PARENT_SCOPE)
endfunction()

set(AUTOTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ps2autotests/tests)
# Every vendored ELF that comes with recorded output, so a test category cannot
# be left unrun by forgetting to name it here.
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

# Each ELF is translated to C++, compiled against the same handlers the
# interpreter uses, and run through the same driver.
foreach(elf ${AUTOTEST_ELFS})
    file(RELATIVE_PATH relative ${AUTOTEST_DIR} ${elf})
    string(REGEX REPLACE "\\.elf$" "" relative ${relative})
    string(REPLACE "/" "." name ${relative})
    set(stem ${CMAKE_CURRENT_BINARY_DIR}/translated/${name})
    translated_unit_names(generated ${stem} ${TRANSLATED_UNITS})
    add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                       COMMAND ${CMAKE_COMMAND} -E make_directory
                               ${CMAKE_CURRENT_BINARY_DIR}/translated
                       COMMAND translate_elf ${elf} ${stem} ${TRANSLATED_UNITS}
                       DEPENDS translate_elf ${elf}
                       COMMENT "Translating ${name}")
    add_executable(translated_${name} tests/test_translated.cpp ${generated})
    target_include_directories(translated_${name} PRIVATE
                               ${CMAKE_CURRENT_BINARY_DIR}/translated)
    target_link_libraries(translated_${name} PRIVATE uee spdlog::spdlog)
    add_test(NAME translated.${name}
             COMMAND translated_${name} ${elf} ${AUTOTEST_DIR}/${relative}.expected)
endforeach()

# Failures someone is already working on. They still run, and a fixed one turns
# the suite red until it is taken off this list.
set(KNOWN_FAILURES vu.lower.efu)
foreach(name ${KNOWN_FAILURES})
    set_tests_properties(${name} translated.${name} PROPERTIES WILL_FAIL TRUE)
endforeach()

file(GLOB IOP_AUTOTEST_IRXS ${AUTOTEST_DIR}/cpu/iop/*.irx)
foreach(irx ${IOP_AUTOTEST_IRXS})
    file(RELATIVE_PATH relative ${AUTOTEST_DIR} ${irx})
    string(REGEX REPLACE "\\.irx$" "" relative ${relative})
    string(REPLACE "/" "." name ${relative})
    add_test(NAME ${name} COMMAND test_iop_autotest ${irx} ${AUTOTEST_DIR}/${relative}.expected)

    # The module is named after itself in the generated source, so the name has
    # to be spellable as a C++ identifier.
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" TRANSLATED_MODULE ${name})
    set(stem ${CMAKE_CURRENT_BINARY_DIR}/translated/${name})
    translated_unit_names(generated ${stem} ${TRANSLATED_UNITS})
    add_custom_command(OUTPUT ${generated} ${stem}.decls.h
                       COMMAND ${CMAKE_COMMAND} -E make_directory
                               ${CMAKE_CURRENT_BINARY_DIR}/translated
                       COMMAND translate_irx ${irx} ${stem} ${TRANSLATED_MODULE}
                               ${TRANSLATED_UNITS}
                       DEPENDS translate_irx ${irx}
                       COMMENT "Translating ${name}")
    set(driver ${CMAKE_CURRENT_BINARY_DIR}/translated/${name}.main.cpp)
    configure_file(tests/test_iop_translated.cpp.in ${driver} @ONLY)
    add_executable(translated_${name} ${driver} ${generated})
    target_include_directories(translated_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
                               ${CMAKE_CURRENT_BINARY_DIR}/translated)
    target_link_libraries(translated_${name} PRIVATE uee spdlog::spdlog)
    add_test(NAME translated.${name}
             COMMAND translated_${name} ${irx} ${AUTOTEST_DIR}/${relative}.expected)
endforeach()

# IOP guest tests are IRX modules built with the IOP toolchain and run through
# the same driver as the vendored IOP autotests. Their expected output is
# derived from the resident IOP services recorded in the module IDBs.
find_program(IOP_GCC mipsel-none-elf-gcc HINTS $ENV{PS2DEV}/iop/bin)
# Cached on the configure that finds it, so a later one without PS2SDK in the
# environment keeps the guest tests rather than quietly dropping them.
if(NOT PS2SDK AND EXISTS "$ENV{PS2SDK}/iop/startup/linkfile")
    set(PS2SDK "$ENV{PS2SDK}" CACHE PATH "ps2sdk the guest tests build against")
endif()
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

# Guest tests are ps2sdk programs built with the PS2 toolchain and run through
# the same driver as the vendored autotests. Their expected output is derived
# from the kernel behaviour recorded in the IDBs, not captured from hardware.
find_program(EE_GCC mips64r5900el-ps2-elf-gcc
             HINTS $ENV{PS2DEV}/ee/bin /usr/local/ps2dev/ee/bin)
if(EE_GCC AND EXISTS "${PS2SDK}/ee/startup/linkfile")
    foreach(guest alarm thread_sema coverage degrade
                  delay priority thread_state sema intc sif_regs syscall_hook machine)
        set(elf ${CMAKE_CURRENT_BINARY_DIR}/guest/${guest}.elf)
        add_custom_command(OUTPUT ${elf}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/guest
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
    endforeach()
else()
    message(STATUS "guest tests skipped: EE_GCC=${EE_GCC} PS2SDK=${PS2SDK}")
endif()

# Not in the repository: an optional frontend based on sky2hook.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/frontends/sky2hook)
    add_subdirectory(frontends/sky2hook)
endif()
