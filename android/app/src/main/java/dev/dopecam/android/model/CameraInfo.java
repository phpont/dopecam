package dev.dopecam.android.model;

public final class CameraInfo {
    public final String id;
    public final int lensFacing;
    public final float zoomMin;
    public final float zoomMax;
    public final boolean logical;

    public CameraInfo(String id, int lensFacing, float zoomMin, float zoomMax, boolean logical) {
        this.id = id;
        this.lensFacing = lensFacing;
        this.zoomMin = zoomMin;
        this.zoomMax = zoomMax;
        this.logical = logical;
    }
}
