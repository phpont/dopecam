package dev.dopecam.android.model;

public enum Preset {
    BUDGET("budget", 1280, 720, 30, 2_500_000, true),
    NORMAL("normal", 1920, 1080, 30, 6_000_000, true),
    QUALITY("quality", 1920, 1080, 30, 12_000_000, false);

    public final String wireName;
    public final int width;
    public final int height;
    public final int fps;
    public final int bitrate;
    public final boolean useVideoCall;

    Preset(String wireName, int width, int height, int fps, int bitrate, boolean useVideoCall) {
        this.wireName = wireName;
        this.width = width;
        this.height = height;
        this.fps = fps;
        this.bitrate = bitrate;
        this.useVideoCall = useVideoCall;
    }

    public static Preset fromWire(String value) {
        for (Preset preset : values()) {
            if (preset.wireName.equalsIgnoreCase(value)) {
                return preset;
            }
        }
        throw new IllegalArgumentException("Unknown preset: " + value);
    }
}
