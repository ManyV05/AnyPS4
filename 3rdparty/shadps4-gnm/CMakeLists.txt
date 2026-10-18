# libSceGnmDriver_padebug.prx: shadPS4's Gnm HLE and PM4 command processor, headless.
# Expects ORBIS_UNPATCHED_DIR, ORBIS_PROSPERO_LIBS and ORBIS_LIBSCEGNMDRIVER_PADEBUG_COMPLEMENT
# from core/libs/orbis/CMakeLists.txt, and the orbis_libc / orbis_libkernel targets.

add_library(orbis_libSceGnmDriver_padebug SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/amdgpu/liverpool.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/amdgpu/regs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/gnmdriver/gnmdriver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Logging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/KernelBridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/adapter/Module.cpp
        ${ORBIS_LIBSCEGNMDRIVER_PADEBUG_COMPLEMENT}
        $<TARGET_OBJECTS:orbis_hle_common>)

target_include_directories(orbis_libSceGnmDriver_padebug PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shims
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/fmt/include
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/magic_enum/include
        ${ORBIS_PROSPERO_LIBS}
        ${ORBIS_PROSPERO_LIBS}/orbis)

target_compile_features(orbis_libSceGnmDriver_padebug PRIVATE cxx_std_23)
target_compile_definitions(orbis_libSceGnmDriver_padebug PRIVATE
        ANYPS4_HEADLESS=1
        FMT_HEADER_ONLY=1
        _WIN32_WINNT=0x0A00
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
target_link_libraries(orbis_libSceGnmDriver_padebug PRIVATE orbis_libkernel orbis_libc)

set_target_properties(orbis_libSceGnmDriver_padebug PROPERTIES
        PREFIX ""
        OUTPUT_NAME libSceGnmDriver_padebug
        SUFFIX .prx
        RUNTIME_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implib)

if(MINGW)
    target_compile_options(orbis_libSceGnmDriver_padebug PRIVATE -fno-asynchronous-unwind-tables -Wno-unused-parameter)
    add_custom_command(TARGET orbis_libSceGnmDriver_padebug POST_BUILD
            COMMAND "${CMAKE_OBJCOPY}" --rename-section .eh_frame=.ehfram "$<TARGET_FILE:orbis_libSceGnmDriver_padebug>"
            VERBATIM)
endif()
