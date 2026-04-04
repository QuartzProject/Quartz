quartz_module(quartz_lsp SOURCES Server.cpp Handlers.cpp Workspace.cpp Query.cpp Complete.cpp Locate.cpp Builtins.cpp
    Imports.cpp Actions.cpp Walk.cpp Index.cpp Navigate.cpp Signature.cpp Hints.cpp Tokens.cpp Symbols.cpp Json.cpp
    Transport.cpp Text.cpp DEPS quartz quartz_format)
target_include_directories(quartz_lsp PRIVATE ${PROJECT_SOURCE_DIR}/cli)
add_executable(quartz-lsp main.cpp)
target_link_libraries(quartz-lsp PRIVATE quartz_lsp)
