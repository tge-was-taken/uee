cmake_minimum_required(VERSION 3.16)
project(ps2gl_capture CXX)

# ps2gl, its glut and ps2stuff ship prebuilt in the ps2dev toolchain's ports.
add_executable(lesson5_digest lesson5_digest.cpp framedigest.cpp)

target_include_directories(lesson5_digest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    $ENV{PS2SDK}/ports/include)

target_link_directories(lesson5_digest PRIVATE $ENV{PS2SDK}/ports/lib)

target_link_libraries(lesson5_digest PRIVATE ps2glut ps2gl ps2stuff dma pad)

set_target_properties(lesson5_digest PROPERTIES SUFFIX ".elf")
