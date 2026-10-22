plugins {
    id("com.android.application")
}

android {
    namespace = "com.camara.obscam"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.camara.obscam"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Firma con la clave debug para poder instalar el release sin keystore propio.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
