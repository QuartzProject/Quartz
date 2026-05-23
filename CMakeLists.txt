cmake_minimum_required(VERSION 3.28)
project(Quartz VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(QuartzOptions)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif()

add_library(quartz_doctest INTERFACE)
target_include_directories(quartz_doctest SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party)

add_library(quartz_monocypher STATIC third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c)
target_include_directories(quartz_monocypher SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_compile_options(quartz_monocypher PRIVATE -ffp-contract=off)

# zstd 1.5.7 (BSD), library only, without assembly or threads so it builds the same on every target (D67).
file(GLOB QUARTZ_ZSTD_SOURCES ${PROJECT_SOURCE_DIR}/third_party/zstd/common/*.c ${PROJECT_SOURCE_DIR}/third_party/zstd/compress/*.c
    ${PROJECT_SOURCE_DIR}/third_party/zstd/decompress/*.c)
add_library(quartz_zstd STATIC ${QUARTZ_ZSTD_SOURCES})
target_include_directories(quartz_zstd SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/zstd)
target_compile_definitions(quartz_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD=0 ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=QZ_)
target_compile_options(quartz_zstd PRIVATE -ffp-contract=off -w)
set_target_properties(quartz_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(src)
if(EMSCRIPTEN)
    # WebAssembly builds the library and the playground module; the tools need processes, sockets and a terminal.
    enable_testing()
    add_subdirectory(wasm)
    return()
endif()
add_subdirectory(cli)
add_subdirectory(lsp)
add_subdirectory(dap)
add_subdirectory(bench)
if(QUARTZ_COMPILER)
    add_subdirectory(compiler)
endif()

if(QUARTZ_FUZZ)
    add_subdirectory(fuzz)
endif()

enable_testing()
add_subdirectory(tests)
