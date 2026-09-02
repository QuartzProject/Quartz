# bindings/jvm built and tested by its Gradle wrapper against this build's libquartz, writing only under build (D156).
set(QUARTZ_JVM_DIR ${PROJECT_SOURCE_DIR}/bindings/jvm)
set(QUARTZ_JVM_BUILD ${CMAKE_BINARY_DIR}/jvm)
file(WRITE ${QUARTZ_JVM_BUILD}/build-dir.init.gradle.kts
    "allprojects {\n    layout.buildDirectory.set(file(\"${QUARTZ_JVM_BUILD}/\${project.name}\"))\n}\n")
if(WIN32)
    set(quartz_gradlew gradlew.bat)
else()
    set(quartz_gradlew gradlew)
endif()
set(QUARTZ_GRADLE ${CMAKE_COMMAND} -E env JAVA_HOME=${QUARTZ_JDK_HOME} ${QUARTZ_JVM_DIR}/${quartz_gradlew}
    --project-dir ${QUARTZ_JVM_DIR} --project-cache-dir ${QUARTZ_JVM_BUILD}/.gradle
    --init-script ${QUARTZ_JVM_BUILD}/build-dir.init.gradle.kts -Pquartz.library=$<TARGET_FILE:quartz_shared>
    -Dorg.gradle.workers.max=4)
if(QUARTZ_ANDROID_ENABLED)
    list(APPEND QUARTZ_GRADLE -Pquartz.ndk=${QUARTZ_ANDROID_NDK} -Pquartz.androidSource=${PROJECT_SOURCE_DIR})
endif()

add_custom_target(quartz-jvm COMMAND ${QUARTZ_GRADLE} assemble DEPENDS quartz_shared
    WORKING_DIRECTORY ${QUARTZ_JVM_DIR} COMMENT "Building the JVM bindings with Gradle" USES_TERMINAL VERBATIM)
add_test(NAME quartz_jvm COMMAND ${QUARTZ_GRADLE} check WORKING_DIRECTORY ${QUARTZ_JVM_DIR})
set_tests_properties(quartz_jvm PROPERTIES LABELS jvm TIMEOUT 1800)
