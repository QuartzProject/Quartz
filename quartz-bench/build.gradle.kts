plugins {
    application
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(25)
    }
}

tasks.withType<JavaCompile>().configureEach {
    options.release = 22
}

dependencies {
    implementation(project(":quartz-java"))
}

application {
    mainClass = "org.quartzlang.jvm.bench.Bench"
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
