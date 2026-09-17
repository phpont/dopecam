# DopeCam v0 architecture

DopeCam v0 is intentionally narrow: one Android phone, one Windows PC, one LAN, video only.
The checkpoint validates the media path and control model before the Windows Frame Server virtual-camera layer is added.

## Design constraints

- No cloud or account.
- No USB path.
- No OBS, FFmpeg, WebRTC, Qt, Electron, AndroidX or Compose runtime.
- No raw-frame processing in DopeCam code on Android.
- H.264 encoding must be hardware accelerated on Android.
- Windows asks the Microsoft H.264 MFT for D3D11-backed NV12 output.
- Rotation, mirroring and scaling stay on the PC.
- Video loss is handled by dropping stale/incomplete frames and requesting a new IDR, never by retransmitting old video.

## Android path

```text
Camera2 logical camera
        |
        v
Camera HAL / ISP
        |
        v
MediaCodec input Surface
        |
        v
hardware H.264 encoder
        |
        v
RTP packetizer
        |
        v
UDP over LAN
```

v0 compiles and targets Android 16 / API 36. Android 17 gates direct LAN TCP/UDP behind a new runtime local-network permission for apps targeting 37+, so that future permission flow remains deferred while this personal checkpoint validates the media pipeline.

The app has two operational states:

- `OFF`: no service.
- `ARMED`: foreground camera service is alive, but Camera2 and MediaCodec are closed until a PC sends `START`.
- `STREAMING`: Camera2, the encoder and RTP sender are active.

The phone UI is deliberately small. Camera, preset and zoom are PC-controlled after the phone is armed.

### Presets

| Preset | Capture target | FPS | H.264 VBR target | Camera use case |
| --- | ---: | ---: | ---: | --- |
| Budget | 1280x720 | 30 | 2.5 Mbps | VIDEO_CALL when exposed |
| Normal | 1920x1080 | 30 | 6 Mbps | VIDEO_CALL when exposed |
| Quality | 1920x1080 | 30 | 12 Mbps | default/high-quality path |

If an exact capture size is unavailable, Android chooses the largest 16:9 MediaCodec output not exceeding the preset target, then falls back to the closest area.

Only hardware-accelerated AVC encoders are accepted. v0 deliberately fails instead of silently using a software encoder.

## Camera selection

Android enumerates Camera2 camera IDs and reports lens facing plus `CONTROL_ZOOM_RATIO_RANGE`.
Windows translates those capabilities into user choices.

For a logical rear camera whose minimum zoom is below 1.0, Windows exposes:

- `Rear Main` at 1.0x.
- `Rear Ultra-wide` at the HAL-advertised minimum zoom.

This keeps lens switching inside the OEM logical-camera pipeline instead of pinning a physical sensor prematurely.
Front and external cameras remain separate Camera2 IDs.

## Network

Three fixed ports are used in v0:

- UDP 39510: one-shot LAN discovery.
- TCP 39511: reliable control channel.
- UDP 39512: RTP/H.264 received by the PC.

Discovery is intentionally simple because DopeCam is personal software. Manual IPv4 entry remains available if broadcast discovery is blocked.

## Video transport

The Android sender produces RTP v2 packets with payload type 96 and a 90 kHz H.264 timestamp clock.
RTP headers are gathered with slices of the MediaCodec output buffer, avoiding an application-level copy of the compressed payload for each UDP packet.
NAL units that fit in one datagram are sent directly. Larger NAL units use RFC 6184 FU-A fragmentation.
The total datagram size is capped at 1200 bytes to avoid depending on IP fragmentation.

SPS/PPS are cached from MediaCodec codec-specific data and sent immediately before an IDR.

The Windows receiver keeps no traditional video jitter buffer. It assembles only the current access unit. A sequence gap, malformed FU-A or oversized access unit causes that frame to be discarded. The decoder is flushed and a new IDR is requested over TCP.

## Windows path

```text
RTP/UDP
   |
   v
Annex-B H.264 access units
   |
   v
Microsoft H.264 Decoder MFT
   |
   v
D3D11 NV12 surface
   |
   v
D3D11 Video Processor
   |   rotate 0/90/180/270
   |   mirror horizontal/vertical
   |   aspect-fit scaling
   v
DXGI swap chain
   |
   v
native Win32 preview
```

The decoder is given the same `IMFDXGIDeviceManager` used by the renderer. v0 requires the Microsoft decoder to expose D3D-backed output samples; it does not silently fall back to a CPU pixel path.

Transforms do not change the Android encoder or network stream. They apply to the next frame on the PC.

## Why virtual camera is not in v0

The modern Windows virtual-camera path uses `MFCreateVirtualCamera` with a registered Custom Media Source identified by CLSID. That adds an in-process COM media-source DLL loaded by Camera Frame Server.

v0 intentionally keeps the requested release shape at exactly APK + EXE and validates the expensive unknowns first: Camera2, H.264 RTP, hardware decode and transforms.
The protocol and media pipeline are separated so the next checkpoint can move the receiver/decoder into `DopeCamVirtualCamera.dll` without changing Android transport semantics.
