plugins {
    kotlin("jvm") version "2.4.21" apply false
    kotlin("multiplatform") version "2.4.21" apply false
    id("com.android.kotlin.multiplatform.library") version "9.5.1" apply false
}

allprojects {
    group = "org.quartzlang"
    version = "0.1.0"
}
