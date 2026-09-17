plugins {
    id("com.android.application")
}

android {
    namespace = "dev.dopecam.android"
    compileSdk = 36

    defaultConfig {
        applicationId = "dev.dopecam.android"
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
