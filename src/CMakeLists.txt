quartz_module(quartz_support SOURCES support/Source.cpp support/Diagnostics.cpp)
quartz_module(quartz_syntax SOURCES syntax/Lexer.cpp syntax/Parser.cpp syntax/ParserExpr.cpp DEPS quartz_support)
quartz_module(quartz_sema SOURCES ir/Types.cpp sema/Declare.cpp sema/CheckExpr.cpp sema/CheckCall.cpp
    sema/CheckStmt.cpp DEPS quartz_syntax)
quartz_module(quartz_ir SOURCES ir/Print.cpp ir/Lower.cpp ir/LowerExpr.cpp ir/Validator.cpp DEPS quartz_sema)
