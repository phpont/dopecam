# DopeCam

A lightweight, local-first phone-to-PC webcam bridge for Android and Windows.

DopeCam turns an Android phone into a Windows webcam over the local network, with hardware H.264 encoding on the phone, native decoding and rendering on Windows, and no cloud relay.

The project started from a simple goal: use my phone as a webcam without USB, accounts, subscriptions, browser runtimes, or sending video through someone else's servers.

It currently works end to end at 30 fps with real browser and desktop camera consumers.

## What it does

```text
Android phone
    |
    | Camera2
    | MediaCodec / H.264
    | RTP over LAN
    v
Windows app
    |
    | Media Foundation decode
    | D3D11 transforms / preview
    | virtual camera output
    v
Meet / Discord / Chrome / other camera consumers
```

The Android app captures and encodes the camera stream using the device's hardware media stack.

The Windows client discovers the phone on the LAN, controls the session, receives and decodes the H.264 stream, applies transforms, renders a preview, and publishes the result as a Windows camera.

## Current features

- Automatic phone discovery on the local network
- Manual IP connection as a fallback
- Front and rear camera selection
- Three quality presets: `Budget`, `Normal`, and `Quality`
- Hardware H.264 encoding on Android
- RTP/H.264 transport over the LAN
- Native H.264 decoding on Windows
- D3D11 preview and video transforms
- Immediate logarithmic zoom control
- 90° rotation
- Horizontal and vertical mirroring
- Windows virtual camera output
- 30 fps output in real browser camera consumers
- Shared DopeCam branding across both platforms
- Tag-based Windows and Android release builds through GitHub Actions

## Why local-first

DopeCam deliberately has no cloud video path.

Video stays between the phone and the PC on the local network.

There is currently:

- no account system
- no telemetry service
- no cloud relay
- no remote video storage
- no third-party media backend

That decision is both a privacy constraint and an architectural one. A local webcam should not need an internet round trip to move video between two devices sitting next to each other.

## Architecture

### Android

The Android side intentionally stays close to the platform.

```text
Camera2
   |
   v
Surface
   |
   v
MediaCodec
   |
   | H.264
   v
RTP sender
   |
   v
LAN
```

Current stack:

- Java 17
- Android platform APIs
- Camera2
- MediaCodec
- hardware H.264 encoding
- foreground camera service
- UDP/TCP networking

The camera feeds the encoder through a surface-based path to avoid unnecessary application-level frame copies.

The phone can remain armed while the screen is off, while camera selection, preset selection, and zoom are controlled from the PC.

### Windows

The Windows client is native C++20.

```text
LAN
 |
 v
UDP discovery / TCP control / RTP video
 |
 v
Media Foundation
 |
 | decoded frames
 v
D3D11
 |
 | transforms + preview
 v
VirtualCameraPublisher
 |
 v
DirectShow camera
```

Current stack:

- C++20
- Win32
- Winsock
- Media Foundation
- D3D11 / DXGI
- DirectShow compatibility backend

There is no Electron, WebView, Qt, or other large UI runtime.

The goal is to keep the client small, responsive, and close to the operating system APIs it depends on.

## Design decisions

DopeCam is built around a few deliberate constraints.

### Prefer latency over perfect frame retention

A live camera should display the freshest useful frame.

If processing falls behind, letting an ever-growing queue accumulate produces a technically complete stream and a terrible webcam.

The pipeline therefore prioritizes low latency rather than preserving every frame at all costs.

### Encode on the phone

Android devices already have dedicated media hardware.

DopeCam uses the platform H.264 encoder instead of moving raw camera frames through application-level CPU buffers before transmission.

### Keep expensive transforms on the PC

Rotation and mirroring happen on Windows.

The phone's primary job is to capture and encode the camera stream efficiently.

### Avoid unnecessary runtime weight

Both applications use native platform UI.

The project intentionally avoids introducing a large cross-platform UI runtime solely to share interface code between two small clients.

### No cloud relay

If the phone and computer are on the same LAN, the shortest useful path is the local network.

The architecture follows that assumption.

### Treat privacy as an architectural property

The absence of a cloud video path is not a settings toggle.

It is part of the current system design.

## Quality presets

DopeCam exposes three user-facing presets instead of requiring manual tuning of every media parameter.

| Preset | Goal |
| --- | --- |
| `Budget` | Minimize resource and network usage |
| `Normal` | Balance quality, latency, and cost |
| `Quality` | Prefer image quality within the current pipeline |

The long-term goal is to make these presets adapt better to measured device, network, battery, and thermal conditions.

## Connection model

DopeCam uses different channels for different responsibilities.

```text
UDP
  └── local device discovery

TCP
  └── session control

RTP / H.264
  └── video transport
```

The PC can discover an armed phone automatically when both devices are on the same network.

Manual IP connection remains available because local-network discovery is not guaranteed to work on every router or network configuration.

## Virtual camera

The transformed stream is published as a Windows camera so existing applications can consume DopeCam without custom integrations.

The current compatibility implementation uses [`tshino/softcam`](https://github.com/tshino/softcam) as the DirectShow camera backend.

The dependency is pinned during builds instead of being vendored into this repository. Build automation fetches the expected revision and applies the DopeCam-specific camera identity and IPC configuration before compiling it.

`tshino/softcam` is licensed under the MIT License.

## Repository layout

```text
android/
    Android camera, encoder, networking, service, and UI

windows/
    Windows networking, media pipeline, preview, transforms,
    and virtual camera publisher

assets/
    Shared branding assets

docs/
    Architecture and implementation notes

.github/workflows/
    CI and release automation
```

## Build from source

### Requirements

#### Windows

- Windows 10 or Windows 11
- Visual Studio / MSVC with C++ tooling
- CMake 3.24+
- x64 build environment

#### Android

- JDK 17
- Android SDK 36
- Android Build Tools 36.0.0
- Gradle 9.6

The Android application currently targets Android 13 and newer.

### Build the Windows client

```powershell
cmake -S .\windows -B .\build\windows -A x64
cmake --build .\build\windows --config Release --parallel
```

The main executable will be available at:

```text
build\windows\Release\DopeCam.exe
```

The virtual-camera backend is built separately and must be registered on Windows before applications can enumerate it as a camera device.

### Build the Android app

```powershell
gradle -p .\android clean :app:assembleDebug --no-daemon
```

The APK will be generated under:

```text
android\app\build\outputs\apk\debug\
```

## CI and releases

CI runs on pushes and pull requests targeting `main`.

It currently verifies:

- Windows C++ build
- DirectShow compatibility backend build
- Android build

Build artifacts are retained by GitHub Actions.

Tags matching:

```text
v*
```

trigger the release workflow.

A release build currently produces:

- Windows x64 client
- DopeCam DirectShow virtual-camera DLL
- Android APK

Android release signing and polished end-user packaging are still future work.

## Known limitations

DopeCam is functional, but it is still an experimental personal project rather than a polished end-user product.

### Virtual-camera startup order

The current DirectShow backend works most reliably when DopeCam is already streaming before another application enumerates available camera devices.

Some browsers and desktop applications cache camera enumeration.

The most reliable sequence is currently:

1. Arm DopeCam on the phone.
2. Start the Windows client.
3. Connect to the phone.
4. Start streaming.
5. Open or restart the application that will use the camera.
6. Select `DopeCam`.

Making the virtual-camera lifecycle independent from consumer startup order is one of the main remaining tasks.

### x64 only

The current DirectShow camera backend targets x64 Windows applications.

32-bit camera consumers are not yet supported.

### Packaging

There is not yet a polished installer.

The current project is primarily intended to be built and tested from source.

## Roadmap

### Reliability

- Make virtual-camera startup independent from consumer enumeration order
- Improve reconnect and recovery behavior after Wi-Fi changes
- Persist useful PC-side settings
- Improve automatic camera discovery behavior across different networks
- Add x86 virtual-camera support where useful

### Performance

- Measure CPU, GPU, memory, bandwidth, battery usage, and thermals for every preset
- Reduce avoidable GPU readback and CPU color conversion
- Add adaptive bitrate and congestion feedback
- Add thermal and battery-aware behavior on Android
- Evaluate 60 fps after the 30 fps pipeline is measured and stable

### Camera controls

- Improve device camera enumeration
- Expose real device-supported zoom ranges
- Expand useful camera controls without turning the interface into a camera-control panel

### UI and UX

- Keep Android and Windows interaction patterns visually related without sharing a heavy UI runtime
- Improve accessibility and keyboard navigation
- Improve high-DPI behavior
- Keep preview rendering event-driven rather than repainting unnecessarily
- Preserve low startup time and small binaries

### Virtual camera

- Revisit the current BGR24 compatibility bridge after the basic lifecycle is stable
- Investigate a leaner native camera backend
- Evaluate a Media Foundation virtual-camera implementation when Windows 11 becomes the supported baseline

### Distribution

- Add a small Windows installer and uninstaller
- Configure Android release signing
- Produce cleaner release packages from a single version tag
- Keep Android and Windows versions synchronized

### Later

- Optional audio support, if it can be added without compromising the lightweight design

## Project status

DopeCam is actively developed.

The current `v0.x` line represents experimental checkpoints rather than a stable public API or compatibility contract.

Android and Windows versions move together, and one Git tag represents one matching cross-platform release.

## What I wanted to explore

DopeCam started as a practical tool for myself, but it also became an excuse to explore a few questions I find interesting:

- How little software is actually necessary to turn a modern phone into a useful webcam?
- Which work belongs on the phone, and which belongs on the PC?
- How much latency can be removed by designing the pipeline around live interaction instead of file-style media processing?
- Can two platform-native applications still feel like one product without introducing a shared runtime?
- What does a webcam architecture look like when local processing and privacy are defaults rather than optional features?

Those questions have influenced the architecture more than any particular framework or library.

## Author

Built by [Paulo Pontarolo](https://github.com/phpont).
