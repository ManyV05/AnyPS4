# libSceGnmDriver_padebug.prx: shadPS4's Gnm HLE, PM4 command processor and Vulkan renderer.
# Expects ORBIS_UNPATCHED_DIR, ORBIS_PROSPERO_LIBS and ORBIS_LIBSCEGNMDRIVER_PADEBUG_COMPLEMENT
# from core/libs/orbis/CMakeLists.txt, and the orbis_libc / orbis_libkernel targets.
# External libraries come from a shadPS4 checkout (ANYPS4_SHADPS4_EXTERNALS); see VENDOR.md.

set(ANYPS4_SHADPS4_EXTERNALS "${CMAKE_CURRENT_SOURCE_DIR}/externals" CACHE PATH
        "directory with the vendored shadPS4 externals (Boost, Vulkan-Headers, VMA, sirit, glslang, Zydis, xxHash, half, robin-map, xbyak, gcn, json, miniz)")
set(EXT ${ANYPS4_SHADPS4_EXTERNALS})
set(EXTBIN ${CMAKE_BINARY_DIR}/externals)

enable_language(C)

if(NOT TARGET Boost::headers)
    add_subdirectory(${EXT}/ext-boost ${EXTBIN}/boost)
endif()
if(NOT TARGET Vulkan::Headers)
    set(VULKAN_HEADERS_ENABLE_MODULE OFF)
    add_subdirectory(${EXT}/vulkan-headers ${EXTBIN}/vulkan-headers)
endif()
if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
    add_subdirectory(${EXT}/vma ${EXTBIN}/vma)
endif()
if(NOT TARGET glslang)
    set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "")
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "")
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "")
    set(ENABLE_CTEST OFF CACHE BOOL "")
    set(ENABLE_HLSL OFF CACHE BOOL "")
    set(BUILD_EXTERNAL OFF CACHE BOOL "")
    set(ENABLE_OPT OFF CACHE BOOL "")
    add_subdirectory(${EXT}/glslang ${EXTBIN}/glslang)
    file(COPY ${EXT}/glslang/SPIRV DESTINATION ${EXTBIN}/glslang-include/glslang FILES_MATCHING PATTERN "*.h")
endif()
if(NOT TARGET tsl::robin_map)
    add_subdirectory(${EXT}/robin-map ${EXTBIN}/robin-map)
endif()
if(NOT TARGET xbyak::xbyak)
    add_subdirectory(${EXT}/xbyak ${EXTBIN}/xbyak)
endif()
if(NOT TARGET Zydis::Zydis)
    option(ZYDIS_BUILD_TOOLS "" OFF)
    option(ZYDIS_BUILD_EXAMPLES "" OFF)
    option(ZYDIS_BUILD_SHARED_LIB "" OFF)
    add_subdirectory(${EXT}/zydis ${EXTBIN}/zydis)
endif()
if(NOT TARGET sirit)
    add_subdirectory(${EXT}/sirit ${EXTBIN}/sirit)
endif()
if(NOT TARGET gcn)
    add_subdirectory(${EXT}/gcn ${EXTBIN}/gcn)
endif()
if(NOT TARGET xxhash)
    add_library(xxhash STATIC ${EXT}/xxhash/xxhash.c)
    target_include_directories(xxhash PUBLIC ${EXT}/xxhash)
endif()
if(NOT TARGET miniz)
    add_subdirectory(${EXT}/miniz ${EXTBIN}/miniz)
endif()

file(GLOB_RECURSE ANYPS4_RENDERER_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/renderer_vulkan/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/texture_cache/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/buffer_cache/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shader_recompiler/*.cpp)

add_library(orbis_libSceGnmDriver_padebug SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/amdgpu/liverpool.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/amdgpu/regs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/amdgpu/tiling.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/page_manager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/cache_storage.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/gnmdriver/gnmdriver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/decoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/io_file.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/ntapi.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/number_utils.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/recursive_lock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/signal_context.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/spin_lock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/string_util.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/native_clock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/rdtsc.cpp
        ${ANYPS4_RENDERER_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Logging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/KernelBridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Module.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Signals.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/PathUtil.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Renderer.cpp
        ${ORBIS_LIBSCEGNMDRIVER_PADEBUG_COMPLEMENT}
        $<TARGET_OBJECTS:orbis_hle_common>)

target_include_directories(orbis_libSceGnmDriver_padebug PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shims
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/fmt/include
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/magic_enum/include
        ${EXT}/half/include
        ${EXT}/json/include
        ${EXT}/glslang
        ${EXTBIN}/glslang-include
        ${ORBIS_PROSPERO_LIBS}
        ${ORBIS_PROSPERO_LIBS}/orbis)

target_compile_features(orbis_libSceGnmDriver_padebug PRIVATE cxx_std_23)
target_compile_definitions(orbis_libSceGnmDriver_padebug PRIVATE
        ANYPS4_HEADLESS=1
        FMT_HEADER_ONLY=1
        _WIN32_WINNT=0x0A00
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(orbis_libSceGnmDriver_padebug PRIVATE
        orbis_libkernel orbis_libc
        Boost::headers Vulkan::Headers GPUOpen::VulkanMemoryAllocator
        sirit glslang SPIRV tsl::robin_map xbyak::xbyak Zydis::Zydis xxhash gcn miniz mincore)

set_target_properties(orbis_libSceGnmDriver_padebug PROPERTIES
        PREFIX ""
        OUTPUT_NAME libSceGnmDriver_padebug
        SUFFIX .prx
        RUNTIME_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implib)

if(MINGW)
    target_compile_options(orbis_libSceGnmDriver_padebug PRIVATE -fno-asynchronous-unwind-tables -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers -Wno-template-body -Wa,-mbig-obj)
    add_custom_command(TARGET orbis_libSceGnmDriver_padebug POST_BUILD
            COMMAND "${CMAKE_OBJCOPY}" --rename-section .eh_frame=.ehfram "$<TARGET_FILE:orbis_libSceGnmDriver_padebug>"
            VERBATIM)
endif()
