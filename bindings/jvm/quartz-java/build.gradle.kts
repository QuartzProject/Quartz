import java.nio.file.Files

plugins {
    `java-library`
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(25)
    }
    withSourcesJar()
}

tasks.withType<JavaCompile>().configureEach {
    options.release = 22
    options.encoding = "UTF-8"
    options.compilerArgs.addAll(listOf("-Xlint:all,-restricted", "-Werror"))
}

dependencies {
    testImplementation(platform("org.junit:junit-bom:5.14.3"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

val hostPlatform = run {
    val os = System.getProperty("os.name").lowercase()
    val arch = when (val a = System.getProperty("os.arch").lowercase()) {
        "amd64", "x86_64" -> "x86_64"
        "aarch64", "arm64" -> "aarch64"
        else -> a
    }
    val family = when {
        os.startsWith("windows") -> "windows"
        os.startsWith("mac") -> "macos"
        else -> "linux"
    }
    "$family-$arch"
}
val hostLibName = when {
    hostPlatform.startsWith("windows") -> "quartz.dll"
    hostPlatform.startsWith("macos") -> "libquartz.dylib"
    else -> "libquartz.so"
}

// Every libquartz the jar carries, laid out as natives/<os>-<arch>/<file>; CI fills it from each platform's build.
val nativesDir = layout.buildDirectory.dir("natives")
val builtNative = layout.buildDirectory.dir("native-host")
val quartzSource = providers.gradleProperty("quartz.source")
val nativeBuild = layout.buildDirectory.dir("native-build")

val configureNative = tasks.register<Exec>("configureNative") {
    description = "Configures libquartz from -Pquartz.source=<Quartz checkout>."
    onlyIf { quartzSource.isPresent }
    val staticRuntime = if (hostPlatform.startsWith("linux")) "-static-libstdc++ -static-libgcc -s" else ""
    commandLine(
        "cmake", "-S", quartzSource.getOrElse("."), "-B", nativeBuild.get().asFile.path, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        "-DQUARTZ_SHARED=ON", "-DCMAKE_SHARED_LINKER_FLAGS=$staticRuntime",
    )
}

val buildNative = tasks.register<Exec>("buildNative") {
    description = "Builds libquartz for this host from -Pquartz.source and adds it to the jar's natives."
    onlyIf { quartzSource.isPresent }
    dependsOn(configureNative)
    commandLine("cmake", "--build", nativeBuild.get().asFile.path, "--target", "quartz_shared")
    doLast {
        val pattern = Regex("(lib)?quartz\\.(so|dylib|dll)(\\..*)?")
        val lib = nativeBuild.get().asFile.walkTopDown().first {
            it.isFile && !Files.isSymbolicLink(it.toPath()) && it.name.matches(pattern)
        }
        val target = builtNative.get().dir("natives/$hostPlatform").asFile
        target.mkdirs()
        lib.copyTo(target.resolve(hostLibName), overwrite = true)
    }
}

val collectNatives = tasks.register<Sync>("collectNatives") {
    description = "Gathers libquartz builds: -Pquartz.natives=<dir with natives/...> and -Pquartz.library=<host lib>."
    mustRunAfter(buildNative)
    into(nativesDir)
    from(builtNative)
    providers.gradleProperty("quartz.natives").orNull?.let { from(it) { include("natives/**") } }
    providers.gradleProperty("quartz.library").orNull?.let { lib ->
        from(lib) {
            into("natives/$hostPlatform")
            rename { hostLibName }
        }
    }
}

sourceSets.main {
    resources.srcDir(collectNatives)
}

tasks.jar {
    manifest {
        attributes(
            "Automatic-Module-Name" to "org.quartzlang.jvm",
            "Enable-Native-Access" to "ALL-UNNAMED",
        )
    }
}

tasks.test {
    useJUnitPlatform()
    jvmArgs("--enable-native-access=ALL-UNNAMED")
    testLogging {
        events("failed")
        showStandardStreams = false
    }
}
