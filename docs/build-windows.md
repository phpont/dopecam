# Local build notes

The repository intentionally contains no bootstrap/build scripts and no generated dependency folders.

## Android

Requirements:

- JDK 17
- Gradle 9.6.0
- Android SDK Platform 36
- Android SDK Build Tools 36.0.0

From repository root in PowerShell, with `ANDROID_HOME` already configured and Android command-line tools installed:

```powershell
& "$env:ANDROID_HOME\cmdline-tools\latest\bin\sdkmanager.bat" "platforms;android-36" "build-tools;36.0.0"
gradle -p .\android :app:assembleDebug --no-daemon
```

Installable APK output:

```text
android\app\build\outputs\apk\debug\app-debug.apk
```

## Windows

Requirements:

- Visual Studio 2022 Build Tools
- Desktop development with C++ workload
- Current Windows 11 SDK
- CMake

From repository root in a Developer PowerShell for VS 2022:

```powershell
cmake -S .\windows -B .\build\windows -A x64
cmake --build .\build\windows --config Release --parallel
```

Executable output:

```text
build\windows\Release\DopeCam.exe
```
