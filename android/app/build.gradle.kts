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
        versionCode = 2
        versionName = "0.2.0"
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
