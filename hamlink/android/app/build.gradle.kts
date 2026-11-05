import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "org.hamlink.app"
    compileSdk = 35
    ndkVersion = "27.0.12077973" // pinned so F-Droid's build server uses the same NDK

    defaultConfig {
        applicationId = "org.hamlink.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 7
        versionName = "0.2.3"
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
        externalNativeBuild { cmake { arguments += listOf("-DANDROID_STL=c++_static") } }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Release signing key: kept OUTSIDE the repository (never commit it). Default location
    // %USERPROFILE%/.hamlink-release/keystore.properties (storeFile, storePassword, keyAlias, keyPassword), or the file
    // named by HAMLINK_KEYSTORE_PROPS. Without it, release builds are unsigned.
    val keyProps = File(System.getenv("HAMLINK_KEYSTORE_PROPS")
        ?: "${System.getProperty("user.home")}/.hamlink-release/keystore.properties")
    val releaseKey = if (keyProps.exists()) Properties().apply { keyProps.inputStream().use { load(it) } } else null
    signingConfigs {
        if (releaseKey != null) create("release") {
            storeFile = file(releaseKey.getProperty("storeFile"))
            storePassword = releaseKey.getProperty("storePassword")
            keyAlias = releaseKey.getProperty("keyAlias")
            keyPassword = releaseKey.getProperty("keyPassword")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (releaseKey != null) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { compose = true }
    // Help text (HELP.md), shared with the Windows app, which embeds the same file.
    sourceSets["main"].assets.srcDir("../../docs/help")
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("com.github.mik3y:usb-serial-for-android:3.8.1")
}
