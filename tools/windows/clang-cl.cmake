# Cross-compiles for 64-bit Windows with clang-cl against a sysroot made by
# make_sysroot.py. The MSVC STL in it needs Clang 20 or newer.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(LLVM_BIN /usr/lib/llvm-21/bin CACHE PATH "LLVM 20 or newer")
set(WINSYSROOT $ENV{HOME}/.local/opt/winsysroot CACHE PATH "The sysroot make_sysroot.py wrote")

set(CMAKE_C_COMPILER ${LLVM_BIN}/clang-cl)
set(CMAKE_CXX_COMPILER ${LLVM_BIN}/clang-cl)
set(CMAKE_LINKER ${LLVM_BIN}/lld-link)
set(CMAKE_AR ${LLVM_BIN}/llvm-lib)
set(CMAKE_RC_COMPILER ${LLVM_BIN}/llvm-rc)
set(CMAKE_MT ${LLVM_BIN}/llvm-mt)
set(CMAKE_ASM_MASM_COMPILER ${LLVM_BIN}/llvm-ml)
set(CMAKE_ASM_MASM_FLAGS_INIT -m64)

file(GLOB msvc_versions ${WINSYSROOT}/VC/Tools/MSVC/*)
list(GET msvc_versions -1 msvc)
file(GLOB sdk_versions ${WINSYSROOT}/Windows\ Kits/10/Include/*)
list(GET sdk_versions -1 sdk_include)
get_filename_component(sdk_version ${sdk_include} NAME)

set(compile_flags "--target=x86_64-pc-windows-msvc /winsysroot \"${WINSYSROOT}\"")
set(CMAKE_C_FLAGS_INIT ${compile_flags})
set(CMAKE_CXX_FLAGS_INIT ${compile_flags})
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "/winsysroot:\"${WINSYSROOT}\" /manifest:no")
endforeach()

# llvm-rc takes neither /winsysroot nor the INCLUDE a developer prompt sets.
set(CMAKE_RC_FLAGS_INIT "-I \"${msvc}/include\" -I \"${sdk_include}/um\" -I \"${sdk_include}/shared\" -I \"${sdk_include}/ucrt\"")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
