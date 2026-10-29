// Built by scripts/build_android.sh, which passes:
//   -Ptlk.native=DIR       arm64-v8a/ holding libmain.so (the game) and libSDL3.so
//   -Psdl.java=DIR         SDL's Java sources, from the same SDL as libSDL3.so
plugins {
    id("com.android.application")
}

fun prop(name: String): String = providers.gradleProperty(name).orNull ?: error("-P$name is required")

android {
    namespace = "uk.callum.tlkrecomp"
    compileSdk = 36
    defaultConfig {
        applicationId = "uk.callum.tlkrecomp"
        minSdk = 28
        targetSdk = 36
        // Android refuses to install a lower version code over a higher one: minutes since 1970
        versionCode = (System.currentTimeMillis() / 60000).toInt()
        versionName = "phase1"
        ndk { abiFilters += "arm64-v8a" }
    }
    sourceSets["main"].apply {
        java.srcDir(prop("sdl.java"))
        jniLibs.srcDir(prop("tlk.native"))
    }
    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    packaging { jniLibs.keepDebugSymbols += "**/*.so" }   // build_android.sh strips them
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
