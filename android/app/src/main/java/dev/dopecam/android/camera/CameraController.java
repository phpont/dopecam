package dev.dopecam.android.camera;

import android.annotation.SuppressLint;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import dev.dopecam.android.codec.H264Encoder;
import dev.dopecam.android.model.Preset;
import dev.dopecam.android.model.StreamConfig;
import dev.dopecam.android.net.RtpH264Sender;

public final class CameraController implements AutoCloseable {
    public static final class StartResult {
        public final int width;
        public final int height;
        public final int fps;
        public final int bitrate;

        public StartResult(int width, int height, int fps, int bitrate) {
            this.width = width;
            this.height = height;
            this.fps = fps;
            this.bitrate = bitrate;
        }
    }

    private final CameraCatalog catalog;
    private final HandlerThread cameraThread = new HandlerThread("DopeCam-Camera");
    private final HandlerThread codecThread = new HandlerThread("DopeCam-Codec");
    private final Handler cameraHandler;
    private final Handler codecHandler;
    private final Executor cameraExecutor;

    private CameraDevice cameraDevice;
    private CameraCaptureSession captureSession;
    private CaptureRequest.Builder captureBuilder;
    private H264Encoder encoder;
    private RtpH264Sender sender;
    private float zoomMin = 1.0f;
    private float zoomMax = 1.0f;
    private int generation;

    public CameraController(CameraCatalog catalog) {
        this.catalog = catalog;
        cameraThread.start();
        codecThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());
        codecHandler = new Handler(codecThread.getLooper());
        cameraExecutor = command -> cameraHandler.post(command);
    }

    @SuppressLint("MissingPermission")
    public StartResult startStream(StreamConfig config, InetAddress pcAddress) throws Exception {
        final int token;
        synchronized (this) {
            stopStreamLocked();
            token = ++generation;
        }

        Size size = catalog.chooseSize(config.cameraId, config.preset);
        CameraCharacteristics characteristics = catalog.characteristics(config.cameraId);
        Range<Float> zr = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
        synchronized (this) {
            zoomMin = zr != null ? zr.getLower() : 1.0f;
            zoomMax = zr != null ? zr.getUpper() : 1.0f;
        }
        float initialZoom = clamp(config.zoom, zr != null ? zr.getLower() : 1.0f, zr != null ? zr.getUpper() : 1.0f);

        Surface encoderSurface;
        try {
            synchronized (this) {
                if (generation != token) {
                    throw new IllegalStateException("Stream start was cancelled");
                }
                sender = new RtpH264Sender(new InetSocketAddress(pcAddress, config.udpPort));
                encoder = new H264Encoder(codecHandler, sender, size, config.preset);
                encoderSurface = encoder.start();
            }
        } catch (Exception e) {
            stopStream();
            throw e;
        }

        CountDownLatch ready = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();

        try {
            catalog.manager().openCamera(config.cameraId, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    synchronized (CameraController.this) {
                        if (generation != token) {
                            camera.close();
                            return;
                        }
                        cameraDevice = camera;
                    }
                    try {
                        configureSession(token, camera, encoderSurface, characteristics, config.preset, initialZoom, ready, failure);
                    } catch (Throwable t) {
                        failure.compareAndSet(null, t);
                        ready.countDown();
                    }
                }

                @Override
                public void onDisconnected(CameraDevice camera) {
                    camera.close();
                    failure.compareAndSet(null, new IllegalStateException("Camera disconnected"));
                    ready.countDown();
                }

                @Override
                public void onError(CameraDevice camera, int error) {
                    camera.close();
                    failure.compareAndSet(null, new IllegalStateException("Camera error: " + error));
                    ready.countDown();
                }
            }, cameraHandler);
        } catch (Exception e) {
            stopStream();
            throw e;
        }

        if (!ready.await(6, TimeUnit.SECONDS)) {
            stopStream();
            throw new IllegalStateException("Timed out while starting camera");
        }
        if (failure.get() != null) {
            Throwable t = failure.get();
            stopStream();
            if (t instanceof Exception) {
                throw (Exception) t;
            }
            throw new IllegalStateException(t);
        }

        return new StartResult(size.getWidth(), size.getHeight(), config.preset.fps, config.preset.bitrate);
    }

    private void configureSession(
            int token,
            CameraDevice camera,
            Surface encoderSurface,
            CameraCharacteristics characteristics,
            Preset preset,
            float initialZoom,
            CountDownLatch ready,
            AtomicReference<Throwable> failure) throws Exception {

        OutputConfiguration output = new OutputConfiguration(encoderSurface);
        if (preset.useVideoCall && catalog.supportsVideoCallUseCase(camera.getId())) {
            try {
                output.setStreamUseCase(CameraMetadata.SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL);
            } catch (IllegalArgumentException ignored) {
                // Some HALs advertise a use case but reject a particular stream combination.
            }
        }

        SessionConfiguration sessionConfig = new SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR,
                Collections.singletonList(output),
                cameraExecutor,
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession session) {
                        synchronized (CameraController.this) {
                            if (generation != token) {
                                session.close();
                                return;
                            }
                            captureSession = session;
                        }
                        try {
                            CaptureRequest.Builder builder = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                            builder.addTarget(encoderSurface);
                            builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, initialZoom);
                            setFps(builder, characteristics, preset.fps);
                            setAf(builder, characteristics);
                            builder.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                                    CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
                            session.setRepeatingRequest(builder.build(), null, cameraHandler);
                            synchronized (CameraController.this) {
                                if (generation == token) {
                                    captureBuilder = builder;
                                }
                            }
                            ready.countDown();
                        } catch (Throwable t) {
                            failure.compareAndSet(null, t);
                            ready.countDown();
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession session) {
                        failure.compareAndSet(null, new IllegalStateException("Camera session configuration failed"));
                        ready.countDown();
                    }
                });
        camera.createCaptureSession(sessionConfig);
    }

    private static void setFps(CaptureRequest.Builder builder, CameraCharacteristics characteristics, int fps) {
        Range<Integer>[] ranges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges == null || ranges.length == 0) {
            return;
        }
        Range<Integer> best = null;
        for (Range<Integer> range : ranges) {
            if (range.getLower() == fps && range.getUpper() == fps) {
                best = range;
                break;
            }
            if (range.contains(fps)) {
                if (best == null || (range.getUpper() - range.getLower()) < (best.getUpper() - best.getLower())) {
                    best = range;
                }
            }
        }
        if (best != null) {
            builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, best);
        }
    }

    private static void setAf(CaptureRequest.Builder builder, CameraCharacteristics characteristics) {
        int[] modes = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
        if (modes == null) {
            return;
        }
        for (int mode : modes) {
            if (mode == CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO) {
                builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                return;
            }
        }
    }

    public synchronized void setZoom(float zoom) throws Exception {
        if (captureSession == null || captureBuilder == null) {
            throw new IllegalStateException("No active stream");
        }
        float value = clamp(zoom, zoomMin, zoomMax);
        captureBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, value);
        captureSession.setRepeatingRequest(captureBuilder.build(), null, cameraHandler);
    }

    public synchronized void requestIdr() {
        if (encoder != null) {
            encoder.requestIdr();
        }
    }

    public synchronized void stopStream() {
        stopStreamLocked();
    }

    private void stopStreamLocked() {
        generation++;
        captureBuilder = null;
        if (captureSession != null) {
            try {
                captureSession.stopRepeating();
            } catch (Exception ignored) {
            }
            captureSession.close();
            captureSession = null;
        }
        if (cameraDevice != null) {
            cameraDevice.close();
            cameraDevice = null;
        }
        if (encoder != null) {
            encoder.stop();
            encoder = null;
        }
        if (sender != null) {
            sender.close();
            sender = null;
        }
    }

    private static float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }

    @Override
    public synchronized void close() {
        stopStreamLocked();
        cameraThread.quitSafely();
        codecThread.quitSafely();
    }
}
