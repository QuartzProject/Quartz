rootProject.name = "quartz-jvm"

pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositories {
        mavenCentral()
    }
}

include("quartz-java", "quartz-kotlin", "quartz-kmp", "quartz-bench")
