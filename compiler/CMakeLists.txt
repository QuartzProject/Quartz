# quartz-compiler: the optional LLVM backend, a shared library the host loads with dlopen (D15).
find_program(QUARTZ_LLVM_CONFIG NAMES llvm-config-23 llvm-config REQUIRED)
execute_process(COMMAND ${QUARTZ_LLVM_CONFIG} --includedir OUTPUT_VARIABLE QUARTZ_LLVM_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${QUARTZ_LLVM_CONFIG} --link-static --libfiles orcjit passes native
    OUTPUT_VARIABLE QUARTZ_LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${QUARTZ_LLVM_CONFIG} --link-static --system-libs orcjit passes native
    OUTPUT_VARIABLE QUARTZ_LLVM_SYSTEM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(QUARTZ_LLVM_LIBS)
separate_arguments(QUARTZ_LLVM_SYSTEM_LIBS)

add_library(quartz_compiler SHARED Api.cpp Codegen.cpp FunctionCodegen.cpp FunctionSupport.cpp Intrinsics.cpp
    ${PROJECT_SOURCE_DIR}/src/ir/Types.cpp)
target_include_directories(quartz_compiler PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_include_directories(quartz_compiler SYSTEM PRIVATE ${QUARTZ_LLVM_INCLUDE})
target_compile_definitions(quartz_compiler PRIVATE __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)
target_link_libraries(quartz_compiler PRIVATE quartz::options ${QUARTZ_LLVM_LIBS} ${QUARTZ_LLVM_SYSTEM_LIBS})
set_target_properties(quartz_compiler PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
# Only the qc_* entry points leave the library; LLVM stays private to it.
target_link_options(quartz_compiler PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,--as-needed
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
set_property(TARGET quartz_compiler APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
