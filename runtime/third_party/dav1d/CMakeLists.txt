# dav1d 1.5.1 (BSD-2-Clause, COPYING): the AV1 decoder of the Switch 2 movie player (src/media/movie_vdec.cpp), 8-bit. The C sources
# are nxnative-penis6's vendored copy (dm/third_party/dav1d, identical to upstream 1.5.1), config/config.h for Windows x64,
# src/win32/thread.c from upstream. The *_tmpl.c files are the bit-depth templates, built once with BITDEPTH=8 as meson does per bit
# depth. With NXRT_DAV1D_ASM (default on when NASM is found) the x86-64 assembly from upstream 1.5.1 (src/x86, src/ext/x86, the 8-bit
# list of upstream src/meson.build) is built too: several times faster, which a 1440p movie needs to play smoothly. config/config.asm is
# what meson writes for NASM on x86_64 Windows.
# NASM: on PATH, in NXRT_NASM_DIR (cache variable) or in $ENV{NX_TOOLCHAIN}/bin, or name the exe with -DNXRT_NASM=<path>.
set(NXRT_NASM_DIR "$ENV{NX_TOOLCHAIN}/bin" CACHE PATH "extra folder searched for nasm.exe")
find_program(NXRT_NASM nasm HINTS "${NXRT_NASM_DIR}")
if(NXRT_NASM)
    set(_d1d_asm_default ON)
else()
    set(_d1d_asm_default OFF)
endif()
option(NXRT_DAV1D_ASM "Build dav1d's x86-64 assembly (needs NASM 2.14+)" ${_d1d_asm_default})
file(GLOB _d1d_src CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
set(_d1d_tmpl ${_d1d_src})
list(FILTER _d1d_tmpl INCLUDE REGEX "_tmpl\\.c$")
set(_d1d_asm "")
if(NXRT_DAV1D_ASM)
    if(NOT NXRT_NASM)
        message(FATAL_ERROR "NXRT_DAV1D_ASM is on but nasm was not found")
    endif()
    list(APPEND _d1d_src "${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cpu.c")
    # Assembled with explicit commands (not CMake's ASM_NASM language), so the project-wide clang-cl flags never reach NASM;
    # the arguments are upstream's nasm generator's (src/meson.build).
    foreach(_a cpuid msac pal refmvs itx_avx512 cdef_avx2 itx_avx2 cdef_sse itx_sse
               cdef_avx512 filmgrain_avx512 ipred_avx512 loopfilter_avx512 looprestoration_avx512 mc_avx512
               filmgrain_avx2 ipred_avx2 loopfilter_avx2 looprestoration_avx2 mc_avx2
               filmgrain_sse ipred_sse loopfilter_sse looprestoration_sse mc_sse)
        set(_in "${CMAKE_CURRENT_SOURCE_DIR}/src/x86/${_a}.asm")
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/asm/${_a}.obj")
        add_custom_command(OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/asm"
            COMMAND "${NXRT_NASM}" -f win64 -I "${CMAKE_CURRENT_SOURCE_DIR}/src/" -I "${CMAKE_CURRENT_SOURCE_DIR}/config/"
                    -MQ "${_out}" -MD "${_out}.d" "${_in}" -o "${_out}"
            DEPENDS "${_in}" "${CMAKE_CURRENT_SOURCE_DIR}/config/config.asm" "${CMAKE_CURRENT_SOURCE_DIR}/src/ext/x86/x86inc.asm"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/x86/filmgrain_common.asm"
            DEPFILE "${_out}.d"
            COMMENT "NASM ${_a}.asm" VERBATIM)
        list(APPEND _d1d_asm "${_out}")
    endforeach()
    set_source_files_properties(${_d1d_asm} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
endif()
add_library(nxrt_dav1d STATIC ${_d1d_src} ${_d1d_asm} "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/thread.c")
set_source_files_properties(${_d1d_tmpl} PROPERTIES COMPILE_DEFINITIONS BITDEPTH=8)
target_include_directories(nxrt_dav1d PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/config" "${CMAKE_CURRENT_SOURCE_DIR}/include/dav1d")
target_include_directories(nxrt_dav1d PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(nxrt_dav1d PRIVATE "$<$<COMPILE_LANGUAGE:C>:NDEBUG;_WIN32_WINNT=0x0A00>")
if(NXRT_DAV1D_ASM)
    target_compile_definitions(nxrt_dav1d PRIVATE $<$<COMPILE_LANGUAGE:C>:NXRT_DAV1D_ASM=1>)
endif()
set_property(TARGET nxrt_dav1d PROPERTY C_STANDARD 11)
# Decoding speed matters in every configuration (a movie plays in real time): optimised, warnings off (third-party code)
target_compile_options(nxrt_dav1d PRIVATE "$<$<COMPILE_LANGUAGE:C>:-w;/O2;-fno-strict-aliasing>")
