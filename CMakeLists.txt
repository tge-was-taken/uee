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

add_library(uee
    src/opcodes.cpp
    src/instruction.cpp
    src/interpreter.cpp
    src/vu0.cpp
    src/vu0_interpreter.cpp
    src/fpu_interpreter.cpp
    src/memory.cpp
    src/elf.cpp
    src/kernel.cpp
    src/sif.cpp
    src/run.cpp)
target_include_directories(uee PUBLIC src)

enable_testing()

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

set(AUTOTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ps2autotests/tests)
file(GLOB AUTOTEST_ELFS
     ${AUTOTEST_DIR}/cpu/ee/*.elf
     ${AUTOTEST_DIR}/cpu/ee_cop0/*.elf
     ${AUTOTEST_DIR}/cpu/ee_fpu/*.elf
     ${AUTOTEST_DIR}/cpu/ee_simd/*.elf
     ${AUTOTEST_DIR}/cpu/vu0_macro/*.elf)
foreach(elf ${AUTOTEST_ELFS})
    file(RELATIVE_PATH relative ${AUTOTEST_DIR} ${elf})
    string(REGEX REPLACE "\\.elf$" "" relative ${relative})
    string(REPLACE "/" "." name ${relative})
    add_test(NAME ${name} COMMAND test_autotest ${elf} ${AUTOTEST_DIR}/${relative}.expected)
endforeach()
