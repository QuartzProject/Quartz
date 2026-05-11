import java.nio.file.Files
import org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("multiplatform")
}

// The native targets link libquartz statically, built here from a Quartz checkout given as -Pquartz.source.
val quartzSource = providers.gradleProperty("quartz.source").orNull
val staticDir = layout.buildDirectory.dir("quartz-static")

val buildQuartzStatic = tasks.register<Exec>("buildQuartzStatic") {
    description = "Builds libquartz_kmp.a (the C interface and everything it needs) from -Pquartz.source."
    val build = staticDir.get().dir("cmake").asFile
    val out = staticDir.get().dir("lib").asFile
    inputs.dir("${quartzSource ?: "."}/src")
    outputs.dir(out)
    commandLine(
        "sh", "-c",
        """
        set -e
        cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_POSITION_INDEPENDENT_CODE=ON > "$2.log"
        cmake --build "$2" --target quartz_c >> "$2.log"
        mkdir -p "$3"
        { echo "create $3/libquartz_kmp.a"; find "$2" -name '*.a' | sed 's/^/addlib /'; echo save; echo end; } | ar -M
        """.trimIndent(),
        "build-quartz", quartzSource ?: ".", build.path, out.path,
    )
}

// A sysroot of links into the host, laid out where Kotlin/Native looks: crt files in usr/lib, libstdc++ in gcc.
val hostSysrootDir = layout.buildDirectory.dir("host-sysroot").get().asFile
val hostSysroot = tasks.register("hostSysroot") {
    description = "Links the host's C runtime and newest GCC into a sysroot for the Kotlin/Native linker."
    outputs.dir(hostSysrootDir)
    doLast {
        val libDir = listOf("/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/usr/lib").map(::File)
            .first { File(it, "crt1.o").isFile }
        val gcc = listOf("/usr/lib/gcc", "/usr/lib64/gcc").map(::File)
            .flatMap { root -> root.listFiles()?.flatMap { it.listFiles()?.toList() ?: emptyList() } ?: emptyList() }
            .filter { File(it, "libstdc++.a").isFile }
            .maxByOrNull { it.name.substringBefore('.').toIntOrNull() ?: 0 }
            ?: throw GradleException("no static libstdc++ on this host (libstdc++-static)")
        hostSysrootDir.deleteRecursively()
        File(hostSysrootDir, "usr").mkdirs()
        fun link(name: String, target: File) =
            Files.createSymbolicLink(File(hostSysrootDir, name).toPath(), target.toPath())
        link("usr/lib", libDir)
        link("usr/lib64", libDir)
        link("usr/include", File("/usr/include"))
        link("lib", File("/lib64").takeIf { it.isDirectory } ?: File("/lib"))
        link("lib64", File("/lib64").takeIf { it.isDirectory } ?: File("/lib"))
        link("gcc", gcc)
    }
}

kotlin {
    explicitApi()
    jvmToolchain(25)

    @OptIn(ExperimentalKotlinGradlePluginApi::class)
    applyDefaultHierarchyTemplate {
        common {
            group("capi") {
                withNative()
            }
        }
    }

    jvm {
        compilerOptions {
            jvmTarget = JvmTarget.JVM_22
        }
    }

    if (quartzSource != null) {
        linuxX64 {
            // libquartz needs a C++23 libstdc++; Kotlin/Native's sysroot has GCC 8 and glibc 2.19, so link on the host's.
            binaries.all {
                linkTaskProvider.configure { dependsOn(hostSysroot) }
                freeCompilerArgs += "-Xoverride-konan-properties=targetSysRoot.linux_x64=${hostSysrootDir.path};" +
                    "libGcc.linux_x64=gcc"
            }
            compilations.getByName("main").cinterops.create("quartz") {
                definitionFile = file("src/nativeInterop/cinterop/quartz.def")
                includeDirs("$quartzSource/include")
                extraOpts("-libraryPath", staticDir.get().dir("lib").asFile.path)
                tasks.named(interopProcessingTaskName) { dependsOn(buildQuartzStatic) }
            }
        }
    } else {
        logger.lifecycle("quartz-kmp: set -Pquartz.source=<Quartz checkout> to build the native targets")
    }

    compilerOptions {
        allWarningsAsErrors = true
    }

    sourceSets {
        commonTest.dependencies {
            implementation(kotlin("test"))
        }
        jvmMain.dependencies {
            api(project(":quartz-java"))
        }
    }
}

tasks.withType<Test>().configureEach {
    jvmArgs("--enable-native-access=ALL-UNNAMED")
    providers.gradleProperty("quartz.library").orNull?.let { systemProperty("quartz.library", it) }
}
