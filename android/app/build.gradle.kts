// Built by scripts/build_android.sh, which passes:
//   -Ptlk.native=DIR       arm64-v8a/ holding libmain.so (the game) and libSDL3.so
//   -Psdl.java=DIR         SDL's Java sources, from the same SDL as libSDL3.so
//   -Ptlk.buildId=ID       libmain.so's TLK_BUILD_ID, which its savestates' sidecars name
//   -Ptlk.version=TEXT     libmain.so's TLK_BUILD_VERSION, the git version
//   -Ptlk.romIdentity=FILE cmake/rom_identity.txt, the dump the launcher accepts
plugins {
    id("com.android.application")
}

fun prop(name: String): String = providers.gradleProperty(name).orNull ?: error("-P$name is required")
fun romKey(key: String): String =
    file(prop("tlk.romIdentity")).readLines().first { it.substringBefore('=').trim() == key }.substringAfter('=').trim()

android {
    namespace = "uk.callum.tlkrecomp"
    compileSdk = 36
    defaultConfig {
        applicationId = "uk.callum.tlkrecomp"
        minSdk = 28
        targetSdk = 36
        // Android refuses to install a lower version code over a higher one: minutes since 1970
        versionCode = (System.currentTimeMillis() / 60000).toInt()
        versionName = prop("tlk.version")
        buildConfigField("String", "BUILD_ID", "\"${prop("tlk.buildId")}\"")
        buildConfigField("String", "ROM_SHA256", "\"${romKey("expected_sha256").lowercase()}\"")
        buildConfigField("long", "ROM_SIZE", "${romKey("rom_size")}L")
        ndk { abiFilters += "arm64-v8a" }
    }
    sourceSets["main"].apply {
        java.srcDir(prop("sdl.java"))
        jniLibs.srcDir(prop("tlk.native"))
    }
    buildFeatures { buildConfig = true }
    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    packaging { jniLibs.keepDebugSymbols += "**/*.so" }   // build_android.sh strips them
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20250517")   // Android's own org.json is a stub off the device
}
