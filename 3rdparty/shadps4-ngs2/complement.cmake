# Included by the generated core/libs/orbis/CMakeLists.txt after the forwarder target is defined.
target_include_directories(orbis_libSceNgs2_complement PRIVATE ${CMAKE_SOURCE_DIR}/3rdparty/shadps4-ngs2/externals/LibAtrac9)
if(MINGW)
    target_compile_options(orbis_libSceNgs2_complement PRIVATE -include common/types.h)
endif()
