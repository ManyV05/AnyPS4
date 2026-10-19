# libSceNgs2.prx: the NGS2 synthesizer of the shadPS4 fork (sampler, pan, submixer, mastering, ATRAC9 decoding).
# Expects ORBIS_UNPATCHED_DIR, ORBIS_PROSPERO_LIBS and ORBIS_LIBSCENGS2_COMPLEMENT from core/libs/orbis/CMakeLists.txt.

enable_language(C)

add_library(orbis_libSceNgs2 SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_custom.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_eq.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_geom.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_impl.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_mastering.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_pan.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_report.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_reverb.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_sampler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_submixer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/band_extension.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/bit_allocation.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/bit_reader.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/decinit.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/decoder.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/huffCodes.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/imdct.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/libatrac9.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/quantization.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/scale_factors.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/tables.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/unpack.c
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9/utility.c
        ${CMAKE_SOURCE_DIR}/3rdparty/shadps4-gnm/adapter/Logging.cpp
        ${ORBIS_LIBSCENGS2_COMPLEMENT}
        $<TARGET_OBJECTS:orbis_hle_common>)

target_include_directories(orbis_libSceNgs2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shims
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/externals/LibAtrac9
        ${CMAKE_SOURCE_DIR}/3rdparty/shadps4-gnm/shims
        ${CMAKE_SOURCE_DIR}/3rdparty/shadps4-gnm/src
        ${CMAKE_SOURCE_DIR}/3rdparty/shadps4-gnm/externals/fmt/include
        ${CMAKE_SOURCE_DIR}/3rdparty/shadps4-gnm/externals/magic_enum/include
        ${ORBIS_PROSPERO_LIBS})

target_compile_features(orbis_libSceNgs2 PRIVATE cxx_std_23)
target_compile_definitions(orbis_libSceNgs2 PRIVATE
        ANYPS4_HEADLESS=1
        FMT_HEADER_ONLY=1
        _WIN32_WINNT=0x0A00
        NOMINMAX
        WIN32_LEAN_AND_MEAN)

target_link_libraries(orbis_libSceNgs2 PRIVATE orbis_libkernel orbis_libc)

set_target_properties(orbis_libSceNgs2 PROPERTIES
        PREFIX ""
        OUTPUT_NAME libSceNgs2
        SUFFIX .prx
        RUNTIME_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implib)

if(MINGW)
    target_compile_options(orbis_libSceNgs2 PRIVATE -fno-asynchronous-unwind-tables -Wno-unused-parameter -Wno-unused-variable
            $<$<COMPILE_LANGUAGE:CXX>:-include;common/types.h>)
    add_custom_command(TARGET orbis_libSceNgs2 POST_BUILD
            COMMAND "${CMAKE_OBJCOPY}" --rename-section .eh_frame=.ehfram "$<TARGET_FILE:orbis_libSceNgs2>"
            VERBATIM)
endif()
