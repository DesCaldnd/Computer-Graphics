plugins {
    id("com.android.application") version "8.7.3" apply false
}

// Build outputs (Gradle + CMake/.cxx) go outside the source tree: E:\Development by default (the C: SSD is full),
// override with -PoxBuildRoot=<dir> or the OX_ANDROID_BUILD_ROOT environment variable.
val oxBuildRoot: String = (findProperty("oxBuildRoot") as String?)
    ?: System.getenv("OX_ANDROID_BUILD_ROOT")
    ?: "E:/Development/OxwaldEngine/build/android"
extra["oxBuildRoot"] = oxBuildRoot
allprojects {
    layout.buildDirectory.set(file("$oxBuildRoot/gradle/${project.name}"))
}
