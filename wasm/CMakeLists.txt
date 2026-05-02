add_library(quartz_wasm_run STATIC Run.cpp)
target_include_directories(quartz_wasm_run PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(quartz_wasm_run PUBLIC quartz)

set(QUARTZ_WASM_LINK -sMODULARIZE=1 -sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8 -sINCOMING_MODULE_JS_API=instantiateWasm,locateFile,print,printErr)

# quartz.js and quartz.wasm: compile and run source, what the playground loads in a Web Worker.
add_executable(quartz-playground Playground.cpp)
set_target_properties(quartz-playground PROPERTIES OUTPUT_NAME quartz SUFFIX .js)
target_include_directories(quartz-playground PRIVATE ${PROJECT_SOURCE_DIR}/cli)
target_link_libraries(quartz-playground PRIVATE quartz_wasm_run)
target_link_options(quartz-playground PRIVATE ${QUARTZ_WASM_LINK} -sEXPORT_NAME=createQuartz
    -sEXPORTED_FUNCTIONS=_qzp_reset,_qzp_source,_qzp_run,_qzp_build,_qzp_module_data)

# quartz-runtime.js and .wasm: runs `.qzb` modules only, without the front end.
add_executable(quartz-runtime Runtime.cpp)
set_target_properties(quartz-runtime PROPERTIES SUFFIX .js)
target_link_libraries(quartz-runtime PRIVATE quartz_wasm_run)
target_link_options(quartz-runtime PRIVATE ${QUARTZ_WASM_LINK} -sEXPORT_NAME=createQuartzRuntime
    -sEXPORTED_FUNCTIONS=_qzr_run,_malloc,_free)

# The program and state tests under Node, reading tests/programs from the host file system.
quartz_test(quartz_programs SOURCES ${PROJECT_SOURCE_DIR}/tests/ProgramTests.cpp DEPS quartz)
add_executable(quartz_state ${PROJECT_SOURCE_DIR}/tests/StateTests.cpp)
target_link_libraries(quartz_state PRIVATE quartz::options quartz_doctest quartz)
target_compile_definitions(quartz_state PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
# Without threads in this build, the one test that runs sandboxes on several threads is left out.
add_test(NAME quartz_state COMMAND quartz_state --test-case-exclude=*threads* WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
foreach(test quartz_programs quartz_state)
    target_link_options(${test} PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB -sEXIT_RUNTIME=1)
endforeach()
# The same programs through quartz.js, the way the playground calls it.
add_test(NAME quartz_wasm_playground COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} ${CMAKE_CURRENT_SOURCE_DIR}/check-programs.mjs
    $<TARGET_FILE:quartz-playground> tests/programs WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
