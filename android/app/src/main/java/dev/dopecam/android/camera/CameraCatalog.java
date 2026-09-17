package dev.dopecam.android.camera;

import android.content.Context;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.media.MediaCodec;
import android.util.Range;
import android.util.Size;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

import dev.dopecam.android.model.CameraInfo;
import dev.dopecam.android.model.Preset;

public final class CameraCatalog {
    private final CameraManager manager;

    public CameraCatalog(Context context) {
        manager = context.getSystemService(CameraManager.class);
    }

    public CameraManager manager() {
        return manager;
    }

    public List<CameraInfo> list() throws Exception {
        List<CameraInfo> result = new ArrayList<>();
        for (String id : manager.getCameraIdList()) {
            CameraCharacteristics c = manager.getCameraCharacteristics(id);
            Integer facing = c.get(CameraCharacteristics.LENS_FACING);
            if (facing == null) {
                continue;
            }

            Range<Float> zoomRange = c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
            float zoomMin = zoomRange != null ? zoomRange.getLower() : 1.0f;
            float zoomMax = zoomRange != null ? zoomRange.getUpper() : 1.0f;

            int[] caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
            boolean logical = caps != null && Arrays.stream(caps)
                    .anyMatch(v -> v == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA);

            result.add(new CameraInfo(id, facing, zoomMin, zoomMax, logical));
        }
        return result;
    }

    public CameraCharacteristics characteristics(String cameraId) throws Exception {
        return manager.getCameraCharacteristics(cameraId);
    }

    public Size chooseSize(String cameraId, Preset preset) throws Exception {
        CameraCharacteristics c = manager.getCameraCharacteristics(cameraId);
        android.hardware.camera2.params.StreamConfigurationMap map =
                c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (map == null) {
            throw new IllegalStateException("Camera has no stream configuration map");
        }

        Size[] sizes = map.getOutputSizes(MediaCodec.class);
        if (sizes == null || sizes.length == 0) {
            throw new IllegalStateException("Camera exposes no MediaCodec output sizes");
        }

        for (Size size : sizes) {
            if (size.getWidth() == preset.width && size.getHeight() == preset.height) {
                return size;
            }
        }

        long targetArea = (long) preset.width * preset.height;
        return Arrays.stream(sizes)
                .filter(s -> Math.abs(((double) s.getWidth() / s.getHeight()) - (16.0 / 9.0)) < 0.03)
                .filter(s -> (long) s.getWidth() * s.getHeight() <= targetArea)
                .max(Comparator.comparingLong(s -> (long) s.getWidth() * s.getHeight()))
                .orElseGet(() -> Arrays.stream(sizes)
                        .min(Comparator.comparingLong(s -> Math.abs(((long) s.getWidth() * s.getHeight()) - targetArea)))
                        .orElse(sizes[0]));
    }

    public boolean supportsVideoCallUseCase(String cameraId) throws Exception {
        long[] values = manager.getCameraCharacteristics(cameraId)
                .get(CameraCharacteristics.SCALER_AVAILABLE_STREAM_USE_CASES);
        if (values == null) {
            return false;
        }
        for (long value : values) {
            if (value == CameraCharacteristics.SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL) {
                return true;
            }
        }
        return false;
    }
}
