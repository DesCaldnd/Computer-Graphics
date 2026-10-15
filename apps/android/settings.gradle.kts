// OxwaldPlayer for Android (GameActivity + the engine as liboxwald_player.so). See docs/guide/37-android.md.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "OxwaldAndroid"
include(":app")
