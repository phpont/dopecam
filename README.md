updated=UI roadmap updated=repository layout updated=minimal UI dependencies decision updated=current UI status updated=checkpoint wording # DopeCam

DopeCam is a personal, lightweight phone-to-PC webcam project. The current checkpoint streams the camera from a Samsung Galaxy Z Flip 7 to Windows over the local network and exposes the result as a Windows camera.

The repository is private for now. There is no public product, installer, support promise, telemetry service, account system, or cloud relay.

## Current status

The current end-to-end checkpoint is functional:

- Android app can be armed and left waiting for the PC.
- Windows can discover the phone automatically on the same LAN.
- Manual IP connection remains available as a fallback.
- The PC can select the camera and one of three quality presets: Budget, Normal, or Quality.
- H.264 video is produced on Android with the hardware media stack and streamed over the LAN.
- Windows receives and decodes the stream with Media Foundation and renders it with D3D11.
- Zoom uses an immediate logarithmic slider on Windows; 90-degree rotation and horizontal/vertical mirroring remain PC-controlled.
- Windows and Android use the shared DopeCam D-cut branding while staying on native platform UI.
- The transformed output can be exposed as the `DopeCam` virtual camera.
- The current virtual-camera backend works in real browser camera consumers at 30 fps.

## Architecture

```text
Galaxy Z Flip 7
    |
    | Camera2 -> MediaCodec -> H.264
    | LAN
    v
Windows DopeCam.exe
    |
    | discovery / control
    | RTP receive
    | Media Foundation decode
    | D3D11 transform / preview
    v
VirtualCameraPublisher
    |
    | shared memory sender
    v
DopeCam DirectShow filter
    |
    v
Chrome / Meet / Discord / other camera consumers
```

### Android

The Android side is deliberately small. It uses platform APIs instead of a heavy UI or media framework.

Current direction:

- Java / Android platform APIs.
- Camera2 for camera access.
- MediaCodec for hardware H.264 encoding.
- Surface-based camera-to-encoder path to avoid unnecessary CPU frame copies.
- LAN-only transport.
- The phone can run armed while the screen is off.
- Camera, preset and zoom are controlled from the PC.

### Windows

The Windows side is native C++20.

Current direction:

- Win32 UI.
- Media Foundation for H.264 decoding.
- D3D11 for rendering and video transforms.
- UDP-based LAN discovery.
- TCP control channel.
- RTP/H.264 video transport.
- No Electron, WebView, Qt or other large UI runtime.

The current machine is Windows 10 build 19045. `MFCreateVirtualCamera` is therefore not available, so the current compatibility backend is DirectShow.

The virtual-camera implementation uses `tshino/softcam`, pinned to commit:

```text
e89a699ed9932c74f57afe4f396be89665967e00
```

The dependency is not vendored into this repository. Build/release automation fetches the pinned source and applies DopeCam-specific branding, CLSID and IPC namespace changes before compiling it.

Current DopeCam DirectShow CLSID:

```text
{E53F2A16-5A12-44CE-A198-862D4BF84725}
```

`tshino/softcam` is MIT licensed.

## Engineering decisions

The project currently follows these constraints:

1. **LAN only.** USB support is intentionally out of scope.
2. **No cloud path.** Video stays on the local network.
3. **Hardware encode on Android.** Raw camera frames should not be pushed through application-level CPU buffers.
4. **Hardware-oriented Windows pipeline.** Media Foundation and D3D11 remain the preferred decode/render path.
5. **Latency over perfect frame retention.** A live camera should prefer fresh frames instead of building an ever-growing queue.
6. **Three presets.** Budget prioritizes efficiency, Normal balances quality and cost, and Quality uses the best settings that fit the current pipeline.
7. **Transforms happen on the PC.** Rotation and mirroring do not need to consume extra phone resources.
8. **Minimal UI dependencies.** Both clients use native platform UI and shared branding without introducing a large framework.
9. **No USB or audio in the current milestone.**
10. **No unnecessary resident process.** The long-term design should consume resources only while the camera is being used.

## Current virtual-camera limitation

The DirectShow filter is currently x64 only.

The current backend also has a lifecycle caveat: the DopeCam sender should already be active before a camera consumer performs its device/capability discovery. Browsers and desktop apps can cache camera enumeration, so after starting DopeCam it may be necessary to fully restart the consumer before `DopeCam` appears.

Current reliable order:

1. Arm DopeCam on the phone.
2. Open `DopeCam.exe`.
3. Discover/connect and press Start.
4. Then open or fully restart the application that will consume the camera.
5. Select `DopeCam`.

This is a known implementation detail to improve, not the intended final UX.

## Repository layout

```text
android/                 Android camera/encoder/server/UI
windows/                 Native Windows receiver/decoder/UI
assets/branding/         Shared DopeCam branding source assets
docs/                    Architecture notes when needed
.github/workflows/       CI and tag-based release automation
```

## Local build

### Windows client

```powershell
cmake -S .\windows -B .\build\windows -A x64
cmake --build .\build\windows --config Release --parallel
```

Main executable:

```text
build\windows\Release\DopeCam.exe
```

The virtual-camera DLL is built separately from the pinned DirectShow dependency and must sit next to `DopeCam.exe` for the sender path. It must also be registered with `regsvr32` for Windows camera consumers.

### Android

The current build uses JDK 17, Gradle 9.6.0, Android platform 36 and build-tools 36.0.0.

```powershell
gradle -p .\android clean :app:assembleDebug --no-daemon
```

Current APK:

```text
android\app\build\outputs\apk\debug\app-debug.apk
```

## CI/CD

`CI` runs for pushes and pull requests against `main`:

- Windows C++ build.
- Pinned DirectShow backend build.
- Android build.
- Build artifacts are retained by GitHub Actions.

`Release` runs for tags matching `v*`:

- Builds the Windows x64 client.
- Builds the pinned DopeCam DirectShow DLL.
- Builds the Android APK.
- Publishes the Windows package, raw executable/DLL, and APK to the same GitHub Release.

The Android asset is intentionally a debug APK at this checkpoint. Release signing is a later milestone.

## Roadmap

### Near term

- Make the virtual camera lifecycle independent of consumer startup order.
- Add x86 DirectShow registration/build for 32-bit consumers.
- Measure CPU, GPU, memory, network throughput and phone battery/thermal cost for all presets.
- Improve camera selection and expose the real device-supported zoom range.
- Persist useful PC-side settings without adding a heavy configuration layer.
- Harden reconnect/recovery when Wi-Fi changes or a stream is interrupted.

### UI / UX

- Keep Windows and Android branding and interaction patterns in sync without adding a shared UI runtime.
- Polish accessibility, keyboard navigation and high-DPI behavior where measurements justify it.
- Keep preview rendering event/frame driven rather than continuously repainting idle UI.
- Preserve low startup time and small binaries.

### Media / performance

- Revisit the DirectShow BGR24 bridge after compatibility is stable.
- Reduce avoidable GPU readback / CPU color-conversion work.
- Investigate a leaner native virtual-camera bridge if measurements justify replacing Softcam.
- Add adaptive bitrate / congestion feedback.
- Add thermal and battery-aware behavior on Android.
- Consider 60 fps only after the 30 fps path is measured and stable.

### Packaging

- Add a small installer/uninstaller for the Windows camera registration.
- Configure Android release signing.
- Produce cleaner release assets from a single version/tag.
- Keep Android and Windows versions in lockstep.

### Later

- Evaluate a Media Foundation virtual-camera backend when Windows 11 becomes the supported baseline.
- Optional audio only if it can be added without compromising the project's lightweight scope.

## Versioning

Android and Windows move together. A single Git tag represents one DopeCam checkpoint and the release assets for both sides.

Initial development line:

```text
v0.x
```

No public compatibility guarantee exists yet.
