# libquartz_jni: the JNI shim and the whole of libquartz in one library, for each Android ABI.
cmake_minimum_required(VERSION 3.28)
project(quartz_jni C CXX)

set(QUARTZ_SOURCE "" CACHE PATH "Quartz checkout")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${QUARTZ_SOURCE} quartz EXCLUDE_FROM_ALL)

add_library(quartz_jni SHARED quartz_jni.c)
target_link_libraries(quartz_jni PRIVATE quartz_c)
set_target_properties(quartz_jni PROPERTIES C_VISIBILITY_PRESET hidden)
target_link_options(quartz_jni PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections -s)
