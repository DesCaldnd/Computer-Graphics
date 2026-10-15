import java.security.MessageDigest

plugins {
    id("com.android.application")
}

fun prop(name: String, fallback: String = ""): String = (findProperty(name) as String?)?.takeIf { it.isNotBlank() } ?: fallback

val repoRoot: File = rootDir.resolve("../..").canonicalFile
val oxBuildRoot: String by rootProject.extra
val ndkVer = "28.2.13676358"
val abis: List<String> = prop("oxAbis", "arm64-v8a,x86_64").split(',').map { it.trim() }.filter { it.isNotEmpty() }
val nativeDebugConfig = prop("oxNativeConfig", "RelWithDebInfo")
val sanitizer = prop("oxSanitizer")
val nativeJobs = prop("oxJobs", "4") // compile jobs (CMake job pool); keep it moderate on shared machines
val vcpkgRoot = prop("oxVcpkgRoot", System.getenv("VCPKG_ROOT") ?: "C:/Programming/vcpkg").replace('\\', '/')
val vcpkgInstalled = prop("oxVcpkgInstalled", "E:/Development/OxwaldEngine/vcpkg_installed_android").replace('\\', '/')
val validationDir = file(prop("oxValidationDir", "E:/Development/Android/vvl"))
// Unstripped liboxwald_player.so is ~760 MB (RelWithDebInfo x86_64): debug APKs carry stripped libraries by default and
// debuggers take the symbols from the CMake build tree (tools/android/debug.ps1, Android Studio). -PoxKeepSymbols=true
// packs them into the APK.
val keepSymbols = prop("oxKeepSymbols", "false").toBoolean()
val pakFile: File? = prop("oxPak").takeIf { it.isNotEmpty() }?.let { file(it) }

android {
    namespace = "com.oxwald.player"
    compileSdk = 35
    ndkVersion = ndkVer

    defaultConfig {
        applicationId = "com.oxwald.player"
        minSdk = 29          // Vulkan 1.1 loader + AAudio everywhere; the engine itself needs a Vulkan 1.2 driver
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.1"
        ndk { abiFilters += abis }
        externalNativeBuild {
            cmake {
                // vcpkg's toolchain chain-loads the NDK toolchain; the triplet follows ANDROID_ABI
                // (arm64-v8a -> arm64-android, x86_64 -> x64-android). Dependencies are installed beforehand by
                // tools/android/vcpkg-install.ps1, so CMake only consumes them (no manifest install during Gradle).
                val ndkDir = "${android.sdkDirectory.path.replace('\\', '/')}/ndk/$ndkVer"
                arguments += listOf(
                    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot/scripts/buildsystems/vcpkg.cmake",
                    "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$ndkDir/build/cmake/android.toolchain.cmake",
                    "-DOX_VCPKG_ANDROID_ROOT=$vcpkgInstalled", // root CMakeLists: <root>/<triplet> per ABI
                    "-DVCPKG_MANIFEST_INSTALL=OFF",
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_PLATFORM=android-29",
                    "-DOX_ANDROID_JOBS=$nativeJobs",
                    "-DOX_ANDROID_SANITIZER=$sanitizer",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                )
                targets += "OxwaldPlayer"
            }
        }
    }

    signingConfigs {
        // Release signing: -PoxKeystore=<file> -PoxKeystorePassword=... -PoxKeyAlias=... -PoxKeyPassword=...
        // (falls back to the debug key so release APKs can be installed for testing).
        create("release") {
            val ks = prop("oxKeystore")
            if (ks.isNotEmpty()) {
                storeFile = file(ks)
                storePassword = prop("oxKeystorePassword")
                keyAlias = prop("oxKeyAlias")
                keyPassword = prop("oxKeyPassword")
            } else {
                val debug = getByName("debug")
                storeFile = debug.storeFile
                storePassword = debug.storePassword
                keyAlias = debug.keyAlias
                keyPassword = debug.keyPassword
            }
        }
    }

    buildTypes {
        getByName("debug") {
            isDebuggable = true
            isJniDebuggable = true
            signingConfig = signingConfigs.getByName("debug")
            externalNativeBuild {
                cmake {
                    // Debug = -O0 for stepping; RelWithDebInfo (default) keeps full symbols but runs at game speed.
                    arguments += listOf("-DCMAKE_BUILD_TYPE=$nativeDebugConfig", "-DOX_ANDROID_REMOTE_CONSOLE_PORT=27015")
                }
            }
        }
        getByName("release") {
            isDebuggable = false
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release", "-DOX_ENABLE_TRACY=OFF")
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = repoRoot.resolve("CMakeLists.txt")
            version = "3.31.6"
            buildStagingDirectory = file("$oxBuildRoot/cxx")
        }
    }

    sourceSets {
        getByName("debug") {
            // Khronos validation layer (tools/android/fetch-validation-layers.ps1): the loader finds layers in the
            // native library directory of debuggable apps.
            if (validationDir.isDirectory) jniLibs.srcDir(validationDir)
            if (sanitizer.isNotEmpty()) {
                jniLibs.srcDir(layout.buildDirectory.dir("generated/sanitizer/jniLibs"))
                resources.srcDir(layout.buildDirectory.dir("generated/sanitizer/resources"))
            }
        }
    }

    packaging {
        jniLibs {
            // wrap.sh (sanitizers) needs the libraries extracted to disk.
            useLegacyPackaging = sanitizer.isNotEmpty()
        }
    }

    androidResources {
        noCompress += listOf("oxpak")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        prefab = true // GameActivity native glue
    }
}

dependencies {
    implementation("androidx.games:games-activity:4.0.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.core:core:1.13.1")
    // appcompat 1.7 brings kotlin-stdlib 1.8 (which absorbed the -jdk7/-jdk8 artifacts) while an older transitive
    // dependency still asks for kotlin-stdlib-jdk8 1.6: align all Kotlin artifacts or checkDuplicateClasses fails.
    implementation(platform("org.jetbrains.kotlin:kotlin-bom:1.8.22"))
}

// ---- engine content -> assets/ ------------------------------------------------------------------------------------
abstract class OxwaldAssetsTask : DefaultTask() {
    @get:InputDirectory abstract val shaderDir: DirectoryProperty
    @get:InputDirectory abstract val uiDir: DirectoryProperty
    @get:Optional @get:InputFile abstract val pak: RegularFileProperty
    @get:OutputDirectory abstract val outputDir: DirectoryProperty

    @TaskAction
    fun run() {
        val out = outputDir.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        shaderDir.get().asFile.copyRecursively(out.resolve("oxwald/engine/shaders"))
        uiDir.get().asFile.copyRecursively(out.resolve("oxwald/engine/ui/resources"))
        if (pak.isPresent) {
            val p = pak.get().asFile
            p.copyTo(out.resolve("project/${p.name}"))
        }
        // AAssetManager cannot list directories: index every file. The stamp makes the app re-extract on change.
        val files = out.walkTopDown().filter { it.isFile }.map { it.relativeTo(out).invariantSeparatorsPath }.sorted().toList()
        val digest = MessageDigest.getInstance("SHA-1")
        for (f in files) {
            digest.update(f.toByteArray())
            digest.update(out.resolve(f).readBytes())
        }
        out.resolve("oxindex.txt").writeText("# generated by :app:generateOxwaldAssets\n" + files.joinToString("\n") + "\n")
        out.resolve("oxbuild.txt").writeText(digest.digest().joinToString("") { "%02x".format(it) } + "\n")
        logger.lifecycle("Oxwald assets: ${files.size} files" + (if (pak.isPresent) " (+ ${pak.get().asFile.name})" else " (no project pak: -PoxPak=...)"))
    }
}

val generateOxwaldAssets = tasks.register<OxwaldAssetsTask>("generateOxwaldAssets") {
    shaderDir.set(repoRoot.resolve("engine/shaders"))
    uiDir.set(repoRoot.resolve("engine/ui/resources"))
    if (pakFile != null) pak.set(pakFile)
    outputDir.set(layout.buildDirectory.dir("generated/oxassets"))
}

// ---- sanitizer runtime + wrap.sh ------------------------------------------------------------------------------
val generateSanitizerFiles = tasks.register("generateSanitizerFiles") {
    val outDir = layout.buildDirectory.dir("generated/sanitizer")
    outputs.dir(outDir)
    doLast {
        val root = outDir.get().asFile
        root.deleteRecursively()
        if (sanitizer.isEmpty()) return@doLast
        val ndk = File(android.sdkDirectory, "ndk/$ndkVer")
        for (abi in abis) {
            val wrap = root.resolve("resources/lib/$abi/wrap.sh")
            wrap.parentFile.mkdirs()
            if (sanitizer == "hwaddress") {
                wrap.writeText("#!/system/bin/sh\nLD_HWASAN=1 exec \"$@\"\n")
            } else {
                val arch = if (abi == "arm64-v8a") "aarch64" else "x86_64"
                val rt = ndk.walkTopDown().firstOrNull { it.name == "libclang_rt.asan-$arch-android.so" }
                    ?: throw GradleException("ASan runtime for $arch not found in $ndk")
                rt.copyTo(root.resolve("jniLibs/$abi/${rt.name}"), overwrite = true)
                wrap.writeText(
                    "#!/system/bin/sh\nHERE=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n" +
                        "export ASAN_OPTIONS=log_to_syslog=false,allow_user_segv_handler=1\n" +
                        "ASAN_LIB=$(ls \$HERE/libclang_rt.asan-*-android.so)\n" +
                        "if [ -f \"\$HERE/libc++_shared.so\" ]; then export LD_PRELOAD=\"\$ASAN_LIB \$HERE/libc++_shared.so\"; " +
                        "else export LD_PRELOAD=\"\$ASAN_LIB\"; fi\n" +
                        "\"$@\"\n"
                )
            }
        }
    }
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(generateOxwaldAssets, OxwaldAssetsTask::outputDir)
        if (variant.buildType == "debug" && keepSymbols) {
            // Unstripped libraries in debug APKs: Android Studio / lldb find symbols without extra search paths.
            variant.packaging.jniLibs.keepDebugSymbols.add("**/*.so")
        }
    }
}

tasks.configureEach {
    if (sanitizer.isNotEmpty() && (name.startsWith("merge") && (name.endsWith("JniLibFolders") || name.endsWith("JavaResource")))) {
        dependsOn(generateSanitizerFiles)
    }
}
