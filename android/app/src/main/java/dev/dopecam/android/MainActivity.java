package dev.dopecam.android;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import dev.dopecam.android.net.DiscoveryServer;
import dev.dopecam.android.net.LanAddress;
import dev.dopecam.android.service.DopecamService;

public final class MainActivity extends Activity {
    private static final int REQUEST_PERMISSIONS = 100;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refreshRunnable = new Runnable() {
        @Override
        public void run() {
            refreshState();
            handler.postDelayed(this, 1000);
        }
    };

    private TextView status;
    private TextView network;
    private Button armButton;
    private boolean armed;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(createContent());
    }

    private LinearLayout createContent() {
        int pad = (int) (24 * getResources().getDisplayMetrics().density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(Color.rgb(245, 245, 245));

        TextView title = new TextView(this);
        title.setText("DopeCam");
        title.setTextSize(28);
        title.setTextColor(Color.BLACK);
        title.setGravity(Gravity.CENTER);
        root.addView(title, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView subtitle = new TextView(this);
        subtitle.setText("Arm the phone, then control the camera from the PC.");
        subtitle.setTextSize(16);
        subtitle.setTextColor(Color.DKGRAY);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        subtitleParams.topMargin = pad;
        root.addView(subtitle, subtitleParams);

        status = new TextView(this);
        status.setText("Disarmed");
        status.setTextSize(18);
        status.setTextColor(Color.BLACK);
        status.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        statusParams.topMargin = pad * 2;
        root.addView(status, statusParams);

        armButton = new Button(this);
        armButton.setText("Arm DopeCam");
        armButton.setOnClickListener(v -> {
            if (armed) {
                disarm();
            } else {
                ensurePermissionsAndArm();
            }
        });
        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        buttonParams.topMargin = pad;
        root.addView(armButton, buttonParams);

        network = new TextView(this);
        network.setTextColor(Color.DKGRAY);
        network.setTextSize(15);
        network.setGravity(Gravity.CENTER);
        network.setTextIsSelectable(true);
        LinearLayout.LayoutParams networkParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        networkParams.topMargin = pad;
        root.addView(network, networkParams);

        TextView device = new TextView(this);
        device.setText("Device: " + Build.MODEL);
        device.setTextColor(Color.GRAY);
        device.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams deviceParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        deviceParams.topMargin = pad;
        root.addView(device, deviceParams);

        return root;
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.removeCallbacks(refreshRunnable);
        handler.post(refreshRunnable);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refreshRunnable);
        super.onPause();
    }

    private void refreshState() {
        armed = DopecamService.isRunning();
        boolean ready = DopecamService.isReady();
        String error = DopecamService.lastError();

        if (ready) {
            status.setText("Armed · ready for PC");
        } else if (armed) {
            status.setText("Arming…");
        } else if (error != null && !error.isBlank()) {
            status.setText("Service error: " + error);
        } else {
            status.setText("Disarmed");
        }
        armButton.setText(armed ? "Disarm DopeCam" : "Arm DopeCam");

        String ip = LanAddress.wifiIpv4(this);
        if (ip == null) {
            network.setText("Wi-Fi IPv4: unavailable\nConnect the phone and PC to the same Wi-Fi network.");
        } else {
            network.setText("Phone IPv4: " + ip
                    + "\nControl port: " + DiscoveryServer.CONTROL_PORT
                    + "\nIf Discover fails, paste this IPv4 into Windows and click Connect IP.");
        }
    }

    private void ensurePermissionsAndArm() {
        boolean camera = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
        boolean notifications = Build.VERSION.SDK_INT < 33
                || checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED;

        if (camera && notifications) {
            arm();
            return;
        }

        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(new String[] {
                    Manifest.permission.CAMERA,
                    Manifest.permission.POST_NOTIFICATIONS
            }, REQUEST_PERMISSIONS);
        } else {
            requestPermissions(new String[] { Manifest.permission.CAMERA }, REQUEST_PERMISSIONS);
        }
    }

    private void arm() {
        Intent intent = new Intent(this, DopecamService.class);
        startForegroundService(intent);
        refreshState();
    }

    private void disarm() {
        stopService(new Intent(this, DopecamService.class));
        armed = false;
        status.setText("Disarmed");
        armButton.setText("Arm DopeCam");
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQUEST_PERMISSIONS) {
            return;
        }

        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            arm();
        } else {
            status.setText("Camera permission is required");
        }
    }
}
