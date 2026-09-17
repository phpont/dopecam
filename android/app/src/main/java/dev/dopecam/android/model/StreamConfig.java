package dev.dopecam.android.model;

public final class StreamConfig {
    public final String cameraId;
    public final Preset preset;
    public final int udpPort;
    public final float zoom;

    public StreamConfig(String cameraId, Preset preset, int udpPort, float zoom) {
        this.cameraId = cameraId;
        this.preset = preset;
        this.udpPort = udpPort;
        this.zoom = zoom;
    }
}
