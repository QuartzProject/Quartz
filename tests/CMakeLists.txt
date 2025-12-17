quartz_test(quartz_programs SOURCES ProgramTests.cpp DEPS quartz)
quartz_test(quartz_format_tests SOURCES FormatTests.cpp DEPS quartz_format)
quartz_test(quartz_state SOURCES StateTests.cpp DEPS quartz)
target_compile_definitions(quartz_state PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
