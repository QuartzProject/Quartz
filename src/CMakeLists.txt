quartz_module(quartz_support SOURCES support/Source.cpp support/Diagnostics.cpp)
quartz_module(quartz_syntax SOURCES syntax/Lexer.cpp syntax/Parser.cpp syntax/ParserExpr.cpp DEPS quartz_support)
quartz_module(quartz_format SOURCES format/Format.cpp format/AstDump.cpp format/Printer.cpp format/PrintDecl.cpp
    format/PrintStmt.cpp format/PrintExpr.cpp DEPS quartz_syntax)
quartz_module(quartz_sema SOURCES ir/Types.cpp sema/Declare.cpp sema/CheckExpr.cpp sema/CheckCall.cpp
    sema/CheckStmt.cpp sema/Generics.cpp sema/Names.cpp DEPS quartz_syntax)
quartz_module(quartz_ir SOURCES ir/Print.cpp ir/Lower.cpp ir/LowerExpr.cpp ir/Optimize.cpp ir/Validator.cpp
    ir/TypeGraph.cpp DEPS quartz_sema)
quartz_module(quartz_vm SOURCES vm/Layout.cpp vm/Compile.cpp vm/Heap.cpp vm/Runtime.cpp vm/Sandbox.cpp vm/Tasks.cpp vm/Migrate.cpp vm/Interp.cpp
    std/Sources.cpp std/Json.cpp std/Toml.cpp std/Yaml.cpp std/Inflate.cpp std/Deflate.cpp std/Binary.cpp
    DEPS quartz_ir)
quartz_module(quartz SOURCES driver/Compiler.cpp DEPS quartz_vm)
# Duplicate the dispatch jump into every handler; without it clang shares one indirect jump (D29).
set_source_files_properties(vm/Interp.cpp PROPERTIES COMPILE_OPTIONS "-mllvm;-tail-dup-indirect-size=200;-mllvm;-tail-dup-size=200")
