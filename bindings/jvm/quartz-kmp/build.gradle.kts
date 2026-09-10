import com.android.build.api.dsl.KotlinMultiplatformAndroidLibraryTarget
import com.android.build.api.variant.KotlinMultiplatformAndroidComponentsExtension
import java.nio.file.Files
import javax.inject.Inject
import org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi
import org.jetbrains.kotlin.gradle.ExperimentalWasmDsl
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.plugin.KotlinPlatformType

plugins {
    kotlin("multiplatform")
    kotlin("plugin.serialization")
}

// The native targets link libquartz statically, built here from a Quartz checkout given as -Pquartz.source.
val quartzSource = providers.gradleProperty("quartz.source").orNull
val staticDir = layout.buildDirectory.dir("quartz-static")

// js and wasmJs load quartz-capi.js and .wasm (Quartz's wasm/ build) as a local npm package made here from -Pquartz.wasm.
val quartzCapi = providers.gradleProperty("quartz.wasm").orNull?.let { dir ->
    val pkg = layout.buildDirectory.dir("quartz-capi-npm").get().asFile
    pkg.mkdirs()
    for (name in listOf("quartz-capi.js", "quartz-capi.wasm")) {
        val from = File(dir, name)
        val to = File(pkg, name)
        if (!to.exists() || to.lastModified() < from.lastModified()) {
            from.copyTo(to, overwrite = true)
        }
    }
    File(pkg, "package.json").writeText(
        """{ "name": "quartz-capi", "version": "0.1.0", "main": "quartz-capi.js", "license": "MIT" }""" + "\n",
    )
    pkg
}

// Android needs the SDK (ANDROID_HOME) and an NDK (-Pquartz.ndk or ANDROID_NDK_HOME) to build libquartz_jni.
val androidSdk = providers.environmentVariable("ANDROID_HOME").orNull
val ndkDir = providers.gradleProperty("quartz.ndk").orElse(providers.environmentVariable("ANDROID_NDK_HOME")).orNull
val androidSource = providers.gradleProperty("quartz.androidSource").orNull ?: quartzSource
val androidEnabled = androidSdk != null && ndkDir != null && androidSource != null
val androidAbis = listOf("arm64-v8a", "x86_64")
if (androidEnabled) {
    apply(plugin = "com.android.kotlin.multiplatform.library")
}

/** Builds libquartz_jni.so (the JNI shim with libquartz inside) for each Android ABI with the NDK. */
abstract class BuildQuartzJni @Inject constructor(private val exec: ExecOperations) : DefaultTask() {
    @get:InputDirectory
    abstract val shim: DirectoryProperty

    @get:Input
    abstract val ndk: Property<String>

    @get:Input
    abstract val quartz: Property<String>

    @get:Input
    abstract val abis: ListProperty<String>

    @get:Internal
    abstract val work: DirectoryProperty

    @get:OutputDirectory
    abstract val output: DirectoryProperty

    @TaskAction
    fun build() {
        for (abi in abis.get()) {
            val dir = work.get().dir(abi).asFile
            exec.exec {
                commandLine(
                    "cmake", "-S", shim.get().asFile.path, "-B", dir.path, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_TOOLCHAIN_FILE=${ndk.get()}/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=$abi",
                    "-DANDROID_PLATFORM=android-26", "-DANDROID_STL=c++_static", "-DQUARTZ_SOURCE=${quartz.get()}",
                )
            }
            exec.exec { commandLine("cmake", "--build", dir.path, "--target", "quartz_jni") }
            val target = output.get().dir(abi).asFile
            target.mkdirs()
            File(dir, "libquartz_jni.so").copyTo(File(target, "libquartz_jni.so"), overwrite = true)
        }
    }
}

val buildQuartzJni = tasks.register<BuildQuartzJni>("buildQuartzJni") {
    shim = layout.projectDirectory.dir("src/androidMain/c")
    ndk = ndkDir ?: ""
    quartz = androidSource ?: ""
    abis = androidAbis
    work = layout.buildDirectory.dir("android-jni")
    output = layout.buildDirectory.dir("android-jniLibs")
}

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
        grep -qs "CMAKE_HOME_DIRECTORY:INTERNAL=$1$" "$2/CMakeCache.txt" || rm -rf "$2"
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
                withCompilations { it.target.platformType == KotlinPlatformType.androidJvm }
                group("web") {
                    withJs()
                    withWasmJs()
                }
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

    if (quartzCapi != null) {
        js {
            nodejs()
        }
        @OptIn(ExperimentalWasmDsl::class)
        wasmJs {
            nodejs()
        }
    } else {
        logger.lifecycle("quartz-kmp: set -Pquartz.wasm=<dir with quartz-capi.js and .wasm> to build js and wasmJs")
    }

    if (androidEnabled) {
        (this as ExtensionAware).extensions.configure<KotlinMultiplatformAndroidLibraryTarget>("android") {
            namespace = "org.quartzlang.kmp"
            compileSdk = 36
            minSdk = 26
            optimization {
                consumerKeepRules.publish = true
                consumerKeepRules.file("src/androidMain/consumer-rules.pro")
            }
            withDeviceTestBuilder { sourceSetTreeName = "test" }.configure {
                instrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
            }
            compilerOptions {
                jvmTarget = JvmTarget.JVM_17
            }
        }
    }

    compilerOptions {
        allWarningsAsErrors = true
    }

    sourceSets {
        commonMain.dependencies {
            api("org.jetbrains.kotlinx:kotlinx-serialization-core:1.11.0")
        }
        commonTest.dependencies {
            implementation(kotlin("test"))
            implementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.11.0")
        }
        jvmMain.dependencies {
            api(project(":quartz-java"))
        }
        if (quartzCapi != null) {
            jsMain.dependencies {
                implementation(npm("quartz-capi", quartzCapi))
            }
            wasmJsMain.dependencies {
                implementation(npm("quartz-capi", quartzCapi))
            }
        }
    }
}

tasks.withType<Test>().configureEach {
    jvmArgs("--enable-native-access=ALL-UNNAMED")
    providers.gradleProperty("quartz.library").orNull?.let { systemProperty("quartz.library", it) }
}

if (androidEnabled) {
    extensions.configure<KotlinMultiplatformAndroidComponentsExtension>("androidComponents") {
        onVariants { variant -> variant.sources.jniLibs?.addGeneratedSourceDirectory(buildQuartzJni) { it.output } }
    }
    kotlin.sourceSets.getByName("androidDeviceTest").dependencies {
        implementation("androidx.test:runner:1.7.0")
        implementation(kotlin("test"))
    }
}
