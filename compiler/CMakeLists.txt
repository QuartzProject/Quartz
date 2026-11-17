# quartz-compiler: the optional LLVM backend, a shared library the host loads with dlopen (D15).
if(WIN32 OR CMAKE_CROSSCOMPILING)
    # Cross builds have no llvm-config for the target; its CMake package says the same.
    find_package(LLVM REQUIRED CONFIG)
    if(NOT LLVM_VERSION_MAJOR EQUAL 23)
        message(FATAL_ERROR "quartz-compiler needs LLVM 23, found ${LLVM_PACKAGE_VERSION}")
    endif()
    set(QUARTZ_LLVM_INCLUDE ${LLVM_INCLUDE_DIRS})
    llvm_map_components_to_libnames(QUARTZ_LLVM_LIBS orcjit passes native)
    set(QUARTZ_LLVM_SYSTEM_LIBS "")
else()
    find_program(QUARTZ_LLVM_CONFIG NAMES llvm-config-23 llvm-config REQUIRED)
    execute_process(COMMAND ${QUARTZ_LLVM_CONFIG} --includedir OUTPUT_VARIABLE QUARTZ_LLVM_INCLUDE
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${QUARTZ_LLVM_CONFIG} --link-static --libfiles orcjit passes native
        OUTPUT_VARIABLE QUARTZ_LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${QUARTZ_LLVM_CONFIG} --link-static --system-libs orcjit passes native
        OUTPUT_VARIABLE QUARTZ_LLVM_SYSTEM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(QUARTZ_LLVM_LIBS)
    separate_arguments(QUARTZ_LLVM_SYSTEM_LIBS)
endif()

add_library(quartz_compiler SHARED Api.cpp Arrays.cpp Builtins.cpp Cache.cpp Closures.cpp Codegen.cpp FunctionCodegen.cpp FunctionSupport.cpp Intrinsics.cpp Loops.cpp
    Tasks.cpp ${PROJECT_SOURCE_DIR}/src/ir/Types.cpp ${PROJECT_SOURCE_DIR}/src/ir/Liveness.cpp
    ${PROJECT_SOURCE_DIR}/src/ir/EnumProof.cpp)
target_include_directories(quartz_compiler PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_include_directories(quartz_compiler SYSTEM PRIVATE ${QUARTZ_LLVM_INCLUDE})
target_compile_definitions(quartz_compiler PRIVATE __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)
target_link_libraries(quartz_compiler PRIVATE quartz::options ${QUARTZ_LLVM_LIBS} ${QUARTZ_LLVM_SYSTEM_LIBS})
set_target_properties(quartz_compiler PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
# Only the qc_* entry points leave the library; LLVM stays private to it.
if(WIN32)
    set_target_properties(quartz_compiler PROPERTIES PREFIX "")
    target_link_options(quartz_compiler PRIVATE -Wl,--gc-sections)
elseif(APPLE)
    target_link_options(quartz_compiler PRIVATE -Wl,-dead_strip
        -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.darwin)
    set_property(TARGET quartz_compiler APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.darwin)
else()
    target_link_options(quartz_compiler PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,--as-needed
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
    set_property(TARGET quartz_compiler APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
endif()
