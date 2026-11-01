quartz_module(quartz_dap SOURCES Session.cpp Variables.cpp Run.cpp Scheduler.cpp Threads.cpp DEPS quartz quartz_lsp)
target_include_directories(quartz_dap PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(quartz_dap PUBLIC Threads::Threads)
