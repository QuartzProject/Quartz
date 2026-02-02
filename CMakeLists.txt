cmake_minimum_required(VERSION 3.28)
project(Quartz VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(QuartzOptions)
find_package(Threads REQUIRED)

add_library(quartz_doctest INTERFACE)
target_include_directories(quartz_doctest SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party)

add_library(quartz_monocypher STATIC third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c)
target_include_directories(quartz_monocypher SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_compile_options(quartz_monocypher PRIVATE -ffp-contract=off)

add_subdirectory(src)
add_subdirectory(cli)
add_subdirectory(lsp)
add_subdirectory(bench)
if(QUARTZ_COMPILER)
    add_subdirectory(compiler)
endif()

if(QUARTZ_FUZZ)
    add_subdirectory(fuzz)
endif()

enable_testing()
add_subdirectory(tests)
