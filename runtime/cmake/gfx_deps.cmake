# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned third-party sources for the graphics shell (S4b). Every archive is fetched by URL and checked against a
# SHA256. The archives are cached in NXRT_DEPS_DIR (shared by checkouts and worktrees, survives a clean build); the
# extracted sources and the SDL build live in the build directory. Licences and pins are listed in THIRD_PARTY_NOTICES.md and README.md.
include(FetchContent)
set(NXRT_DEPS_DIR "$ENV{LOCALAPPDATA}/nxrt/deps" CACHE PATH "Where the pinned third-party sources are downloaded to")
set(FETCHCONTENT_QUIET OFF)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)   # extracted files take the time of extraction

# SDL 3.4.18 (zlib licence), released 2026-10-02. Built from source as a static library.
FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
# Vulkan-Headers vulkan-sdk-1.4.363.0 (Apache-2.0 OR MIT)
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81
    SOURCE_SUBDIR none)
# volk vulkan-sdk-1.4.363.0 (MIT)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=889b90fdcbe9cee7b118339d640c6895b7be5c920d309c44b744692063275b3d
    SOURCE_SUBDIR none)
# VulkanMemoryAllocator v3.4.0 (MIT)
FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6
    SOURCE_SUBDIR none)
# Dear ImGui v1.92.9b (MIT)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8
    SOURCE_SUBDIR none)

# zstd: the shader preload pack (src/gfx/nvn/nvn_shaders.cpp). The library sources only (no CLI, no x86-64 assembly).
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR none)

# SDL: static, only what a game needs.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL_DOCS ON CACHE BOOL "" FORCE)
set(SDL_VENDOR_INFO "nxrt" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(sdl3 vulkan_headers volk vma imgui zstd)

file(GLOB NXRT_ZSTD_SOURCES "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c" "${zstd_SOURCE_DIR}/lib/decompress/*.c")
add_library(nxrt_zstd STATIC ${NXRT_ZSTD_SOURCES})
target_include_directories(nxrt_zstd SYSTEM PUBLIC "${zstd_SOURCE_DIR}/lib")
target_compile_definitions(nxrt_zstd PRIVATE ZSTD_DISABLE_ASM)
set_target_properties(nxrt_zstd PROPERTIES C_STANDARD 11)
# Third-party sources are built without warnings: they are not ours to change, and they bury the warnings of our own code.
if(MSVC)
    set(NXRT_NO_WARNINGS /w)
else()
    set(NXRT_NO_WARNINGS -w)
endif()
target_compile_options(nxrt_zstd PRIVATE ${NXRT_NO_WARNINGS})
if(TARGET SDL3-static)
    target_compile_options(SDL3-static PRIVATE ${NXRT_NO_WARNINGS})
endif()

# volk (loader) next to the Vulkan headers; the Vulkan headers come from the pin, not from the installed SDK.
add_library(nxrt_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(nxrt_volk SYSTEM PUBLIC "${volk_SOURCE_DIR}" "${vulkan_headers_SOURCE_DIR}/include")
target_compile_definitions(nxrt_volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
set_target_properties(nxrt_volk PROPERTIES C_STANDARD 11)

# VMA is header only; one translation unit in src/gfx defines VMA_IMPLEMENTATION.
add_library(nxrt_vma INTERFACE)
target_include_directories(nxrt_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")

include("${CMAKE_CURRENT_LIST_DIR}/imgui_checked.cmake")

# Dear ImGui core + its Vulkan backend (the platform side is fed from the nxrt input queue, so no SDL backend).
add_library(nxrt_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp" "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp" "${CMAKE_CURRENT_BINARY_DIR}/imgui_impl_vulkan_checked.cpp")
target_include_directories(nxrt_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(nxrt_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_compile_options(nxrt_imgui PRIVATE ${NXRT_NO_WARNINGS})
target_link_libraries(nxrt_imgui PUBLIC nxrt_volk)
