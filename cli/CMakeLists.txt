add_executable(quartz-cli main.cpp Fmt.cpp Test.cpp Project.cpp Doc.cpp Repl.cpp)
set_target_properties(quartz-cli PROPERTIES OUTPUT_NAME quartz)
target_link_libraries(quartz-cli PRIVATE quartz quartz_format)
