cmake_minimum_required(VERSION 3.20)

add_executable(anyps4_launcher WIN32
        ${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Bundle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Settings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Process.cpp)
target_compile_features(anyps4_launcher PRIVATE cxx_std_20)
target_compile_definitions(anyps4_launcher PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        ANYPS4_REPO_ROOT="${CMAKE_SOURCE_DIR}"
        ANYPS4_BUILD_DIR="${CMAKE_BINARY_DIR}"
        ANYPS4_LIBS_DIR="${ANYPS4_LIBS_DIR}")
target_include_directories(anyps4_launcher PRIVATE ${CMAKE_SOURCE_DIR}/tools/orbis_impl)
target_link_libraries(anyps4_launcher PRIVATE comctl32 shell32 ole32 user32 gdi32 shlwapi)
if(MINGW)
    target_link_options(anyps4_launcher PRIVATE -static -municode)
endif()
