# DopeCam protocol v1

This protocol is private to the first DopeCam checkpoint. It is versioned so later virtual-camera work does not require an Android rewrite.

## Discovery

PC sends the discovery datagram to the IPv4 limited broadcast and to subnet-directed broadcast addresses for active local interfaces, retrying briefly:

```text
DOPECAM_DISCOVER_V1
```

An armed phone replies to the source address and source port:

```text
DOPECAM_HERE<TAB>MODEL<TAB>39511<TAB>1
```

The last field is protocol version.

## Control connection

TCP port: `39511`.
Encoding: UTF-8.
Framing: one LF-terminated line per message.
Fields are TAB-separated.

Phone greeting after TCP accept:

```text
HELLO<TAB>DOPECAM<TAB>1
```

Only one control client is served at a time in v0. Disconnecting the client stops the active camera stream.

### `CAPS`

Request:

```text
CAPS
```

Response:

```text
CAPS_BEGIN<TAB>1
CAMERA<TAB>cameraId<TAB>lensFacing<TAB>zoomMin<TAB>zoomMax<TAB>logical
...
PRESET<TAB>budget<TAB>1280<TAB>720<TAB>30<TAB>2500000
PRESET<TAB>normal<TAB>1920<TAB>1080<TAB>30<TAB>6000000
PRESET<TAB>quality<TAB>1920<TAB>1080<TAB>30<TAB>12000000
CAPS_END
```

Camera2 lens-facing values are transmitted unchanged: front=0, back=1, external=2.

### `START`

```text
START<TAB>cameraId<TAB>preset<TAB>pcUdpPort<TAB>zoomRatio
```

Success:

```text
OK<TAB>START<TAB>actualWidth<TAB>actualHeight<TAB>fps<TAB>bitrate
```

The phone sends RTP to the IPv4 address of the TCP peer and the UDP port supplied by the PC.

### `STOP`

```text
STOP
```

Success:

```text
OK<TAB>STOP
```

### `SET_ZOOM`

```text
SET_ZOOM<TAB>ratio
```

The phone clamps the value to the active Camera2 zoom range.

### `REQUEST_IDR`

```text
REQUEST_IDR
```

Uses `MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME` on Android.

### `PING`

Returns `PONG`.

### Errors

```text
ERR<TAB>human readable reason
```

## RTP/H.264

PC receive port in v0: UDP `39512`.

- RTP version 2.
- Dynamic payload type 96.
- 90 kHz timestamp clock.
- Random initial RTP sequence number and SSRC.
- No RTP header extensions, CSRC list or RTCP in v0.
- Maximum full UDP datagram: 1200 bytes.
- RFC 6184 single-NAL and FU-A packetization only.
- Marker bit is set on the final packet of the final NAL in one encoded access unit.

Codec-specific SPS/PPS NALs are sent before each IDR with the same RTP timestamp and marker clear.

The receiver does not retransmit video. Any sequence discontinuity that makes an access unit uncertain causes the access unit to be dropped, the H.264 decoder to be flushed, and an IDR to be requested.
