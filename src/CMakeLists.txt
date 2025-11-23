quartz_module(quartz_support SOURCES support/Source.cpp support/Diagnostics.cpp)
quartz_module(quartz_syntax SOURCES syntax/Lexer.cpp syntax/Parser.cpp syntax/ParserExpr.cpp DEPS quartz_support)
