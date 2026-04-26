quartz_module(quartz_support SOURCES support/Source.cpp support/Diagnostics.cpp support/Platform.cpp)
quartz_module(quartz_syntax SOURCES syntax/Lexer.cpp syntax/Parser.cpp syntax/ParserExpr.cpp syntax/DocComment.cpp
    DEPS quartz_support)
quartz_module(quartz_format SOURCES format/Format.cpp format/AstDump.cpp format/Printer.cpp format/PrintDecl.cpp
    format/PrintStmt.cpp format/PrintExpr.cpp DEPS quartz_syntax)
quartz_module(quartz_sema SOURCES ir/Types.cpp sema/Declare.cpp sema/CheckExpr.cpp sema/CheckCall.cpp sema/Lambdas.cpp
    sema/CheckStmt.cpp sema/Generics.cpp sema/Names.cpp sema/CheckBuiltins.cpp DEPS quartz_syntax)
quartz_module(quartz_ir SOURCES ir/Print.cpp ir/Lower.cpp ir/LowerExpr.cpp ir/LowerLambda.cpp ir/LowerBuiltins.cpp ir/Optimize.cpp ir/Validator.cpp
    ir/TypeGraph.cpp ir/ValidateModule.cpp ir/Encode.cpp ir/Decode.cpp ir/Container.cpp ir/Builtins.cpp DEPS quartz_sema quartz_monocypher)
quartz_module(quartz_vm SOURCES vm/Layout.cpp vm/Compile.cpp vm/Heap.cpp vm/Runtime.cpp vm/Sandbox.cpp vm/Tasks.cpp vm/HostValues.cpp vm/Link.cpp vm/Migrate.cpp vm/Math.cpp vm/ReprC.cpp vm/Interp.cpp vm/StrBuiltins.cpp vm/ListBuiltins.cpp
    std/Sources.cpp std/Json.cpp std/Toml.cpp std/Yaml.cpp std/Inflate.cpp std/Deflate.cpp std/Binary.cpp
    DEPS quartz_ir quartz_zstd)
# Suspension-point liveness, shared with the native compiler (D69).
target_sources(quartz_ir PRIVATE ir/Liveness.cpp)
# The native tier loads libquartz_compiler at run time (D15); the core never links LLVM.
target_sources(quartz_vm PRIVATE vm/Tier.cpp vm/NativeRuntime.cpp)
target_link_libraries(quartz_vm PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
quartz_module(quartz SOURCES driver/Compiler.cpp driver/Package.cpp driver/Bind.cpp driver/TomlLite.cpp driver/Index.cpp
    driver/Archive.cpp driver/Resolve.cpp DEPS quartz_vm)
# Duplicate the dispatch jump into every handler; without it clang shares one indirect jump (D29).
set_source_files_properties(vm/Interp.cpp PROPERTIES COMPILE_OPTIONS "-mllvm;-tail-dup-indirect-size=200;-mllvm;-tail-dup-size=200")
# The C interface (include/quartz/quartz.h), static; QUARTZ_SHARED also builds libquartz exporting only qz_*.
quartz_module(quartz_c SOURCES capi/CApi.cpp DEPS quartz)
if(QUARTZ_SHARED)
    add_library(quartz_shared SHARED capi/CApi.cpp)
    set_target_properties(quartz_shared PROPERTIES OUTPUT_NAME quartz CXX_VISIBILITY_PRESET hidden
        VERSION ${PROJECT_VERSION} SOVERSION 0)
    target_compile_definitions(quartz_shared PRIVATE QZ_BUILDING_SHARED)
    target_link_libraries(quartz_shared PRIVATE quartz)
    target_include_directories(quartz_shared PUBLIC ${PROJECT_SOURCE_DIR}/include)
    if(APPLE)
        target_link_options(quartz_shared PRIVATE -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/capi/exports.darwin)
    elseif(NOT WIN32)
        target_link_options(quartz_shared PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/capi/exports.map
            -Wl,--exclude-libs,ALL)
    endif()
endif()
