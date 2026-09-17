package dev.dopecam.android.service;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

import dev.dopecam.android.camera.CameraCatalog;
import dev.dopecam.android.camera.CameraController;
import dev.dopecam.android.net.ControlServer;
import dev.dopecam.android.net.DiscoveryServer;

public final class DopecamService extends Service {
    private static volatile boolean running;
    private static volatile boolean ready;
    private static volatile String lastError = "";
    private static final String CHANNEL_ID = "dopecam-armed";
    private static final int NOTIFICATION_ID = 47;

    private DiscoveryServer discoveryServer;
    private ControlServer controlServer;
    private CameraController cameraController;

    public static boolean isRunning() {
        return running;
    }

    public static boolean isReady() {
        return ready;
    }

    public static String lastError() {
        return lastError;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        running = true;
        ready = false;
        lastError = "";
        createNotificationChannel();
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("DopeCam is armed")
                .setContentText("Waiting for your PC on the local network")
                .setSmallIcon(android.R.drawable.presence_video_online)
                .setOngoing(true)
                .build();
        startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA);

        try {
            CameraCatalog catalog = new CameraCatalog(this);
            cameraController = new CameraController(catalog);
            discoveryServer = new DiscoveryServer();
            controlServer = new ControlServer(catalog, cameraController);
            discoveryServer.start();
            controlServer.start();
            ready = true;
        } catch (Exception e) {
            ready = false;
            String message = e.getMessage();
            lastError = e.getClass().getSimpleName() + (message == null || message.isBlank() ? "" : ": " + message);
            stopSelf();
        }
    }

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "DopeCam armed state",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Shown while DopeCam is available to the PC");
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        ready = false;
        running = false;
        if (controlServer != null) {
            controlServer.close();
            controlServer = null;
        }
        if (discoveryServer != null) {
            discoveryServer.close();
            discoveryServer = null;
        }
        if (cameraController != null) {
            cameraController.close();
            cameraController = null;
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
