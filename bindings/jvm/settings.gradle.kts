rootProject.name = "quartz-jvm"

pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
        google()
    }
}

dependencyResolutionManagement {
    repositories {
        mavenCentral()
        google()
    }
}

include("quartz-java", "quartz-kotlin", "quartz-kmp", "quartz-bench")
