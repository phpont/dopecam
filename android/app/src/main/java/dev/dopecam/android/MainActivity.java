package dev.dopecam.android;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Space;
import android.widget.TextView;
import android.widget.Toast;

import dev.dopecam.android.net.DiscoveryServer;
import dev.dopecam.android.net.LanAddress;
import dev.dopecam.android.service.DopecamService;

public final class MainActivity extends Activity {
    private static final int REQUEST_PERMISSIONS = 100;

    private static final int COLOR_BACKGROUND = Color.rgb(246, 247, 249);
    private static final int COLOR_PRIMARY = Color.rgb(20, 22, 26);
    private static final int COLOR_PRIMARY_PRESSED = Color.rgb(8, 10, 13);
    private static final int COLOR_TEXT = Color.rgb(27, 31, 36);
    private static final int COLOR_SECONDARY = Color.rgb(100, 108, 119);
    private static final int COLOR_MUTED = Color.rgb(127, 136, 148);
    private static final int COLOR_DIVIDER = Color.rgb(221, 225, 230);
    private static final int COLOR_READY = Color.rgb(15, 138, 75);
    private static final int COLOR_ERROR = Color.rgb(180, 35, 24);
    private static final int COLOR_PRESS = Color.rgb(226, 229, 233);

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refreshRunnable = new Runnable() {
        @Override
        public void run() {
            refreshState();
            handler.postDelayed(this, 1000);
        }
    };

    private TextView state;
    private TextView stateDetail;
    private TextView ipValue;
    private TextView networkHint;
    private TextView networkMeta;
    private TextView copyAction;
    private Button armButton;

    private boolean armed;
    private String currentIp;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        configureWindow();
        setContentView(createContent());
    }

    private void configureWindow() {
        Window window = getWindow();

        // targetSdk 36 is edge-to-edge. The root view handles system insets.
        // Do not query WindowInsetsController during early onCreate(), because
        // some Android 16 builds do not have a DecorView yet at this point.
        window.setNavigationBarContrastEnforced(false);
    }

    private View createContent() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        scroll.setBackgroundColor(COLOR_BACKGROUND);
        scroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);

        scroll.setOnApplyWindowInsetsListener((view, windowInsets) -> {
            Insets insets = windowInsets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            view.setPadding(insets.left, insets.top, insets.right, insets.bottom);
            return windowInsets;
        });

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(18), dp(20), dp(22));
        root.setBackgroundColor(COLOR_BACKGROUND);

        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        root.addView(createHeader(), matchWrap());

        state = text("", 15, Typeface.BOLD, COLOR_MUTED);
        LinearLayout.LayoutParams stateParams = matchWrap();
        stateParams.topMargin = dp(20);
        root.addView(state, stateParams);

        stateDetail = text("", 12, Typeface.NORMAL, COLOR_SECONDARY);
        stateDetail.setLineSpacing(0, 1.05f);
        stateDetail.setVisibility(View.GONE);
        LinearLayout.LayoutParams detailParams = matchWrap();
        detailParams.topMargin = dp(4);
        root.addView(stateDetail, detailParams);

        root.addView(divider(), dividerParams(16, 0));

        root.addView(sectionLabel("PHONE"), sectionParams(18));

        LinearLayout ipRow = new LinearLayout(this);
        ipRow.setOrientation(LinearLayout.HORIZONTAL);
        ipRow.setGravity(Gravity.CENTER_VERTICAL);

        ipValue = text("Unavailable", 25, Typeface.BOLD, COLOR_TEXT);
        ipValue.setTextIsSelectable(true);
        LinearLayout.LayoutParams ipParams = new LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f);
        ipRow.addView(ipValue, ipParams);

        copyAction = text("COPY", 12, Typeface.BOLD, COLOR_PRIMARY);
        copyAction.setGravity(Gravity.CENTER);
        copyAction.setClickable(true);
        copyAction.setFocusable(true);
        copyAction.setMinWidth(dp(64));
        copyAction.setPadding(dp(10), dp(9), dp(10), dp(9));

        TypedValue selectable = new TypedValue();
        if (getTheme().resolveAttribute(
                android.R.attr.selectableItemBackgroundBorderless,
                selectable,
                true)) {
            copyAction.setBackgroundResource(selectable.resourceId);
        }

        copyAction.setOnClickListener(v -> copyIp());

        LinearLayout.LayoutParams copyParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        copyParams.leftMargin = dp(12);
        ipRow.addView(copyAction, copyParams);

        root.addView(ipRow, matchWrap());

        networkMeta = text("", 12, Typeface.NORMAL, COLOR_MUTED);
        LinearLayout.LayoutParams metaParams = matchWrap();
        metaParams.topMargin = dp(4);
        root.addView(networkMeta, metaParams);

        networkHint = text(
                "Discover from Windows, or connect manually with this address.",
                12,
                Typeface.NORMAL,
                COLOR_SECONDARY);
        networkHint.setLineSpacing(0, 1.05f);
        LinearLayout.LayoutParams hintParams = matchWrap();
        hintParams.topMargin = dp(8);
        root.addView(networkHint, hintParams);

        root.addView(divider(), dividerParams(18, 0));

        root.addView(sectionLabel("DEVICE"), sectionParams(18));

        TextView deviceName = text(Build.MODEL, 19, Typeface.BOLD, COLOR_TEXT);
        root.addView(deviceName, matchWrap());

        TextView deviceMeta = text(
                "Android " + Build.VERSION.RELEASE
                        + " \u00B7 Camera2 \u00B7 hardware H.264",
                12,
                Typeface.NORMAL,
                COLOR_SECONDARY);
        LinearLayout.LayoutParams deviceMetaParams = matchWrap();
        deviceMetaParams.topMargin = dp(4);
        root.addView(deviceMeta, deviceMetaParams);

        root.addView(divider(), dividerParams(18, 0));

        TextView behavior = text(
                "Camera, quality, zoom and transforms are controlled from Windows. "
                        + "DopeCam does not render a local preview.",
                12,
                Typeface.NORMAL,
                COLOR_SECONDARY);
        behavior.setLineSpacing(0, 1.08f);
        LinearLayout.LayoutParams behaviorParams = matchWrap();
        behaviorParams.topMargin = dp(18);
        root.addView(behavior, behaviorParams);

        Space spacer = new Space(this);
        spacer.setMinimumHeight(dp(24));
        root.addView(spacer, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f));

        armButton = new Button(this);
        armButton.setAllCaps(false);
        armButton.setTextSize(14);
        armButton.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        armButton.setGravity(Gravity.CENTER);
        armButton.setMinHeight(0);
        armButton.setMinWidth(0);
        armButton.setPadding(dp(16), 0, dp(16), 0);
        armButton.setStateListAnimator(null);
        armButton.setOnClickListener(v -> {
            if (armed) {
                disarm();
            } else {
                ensurePermissionsAndArm();
            }
        });

        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(48));
        buttonParams.topMargin = dp(24);
        root.addView(armButton, buttonParams);

        TextView footer = text(
                "Phone and PC must share the same local network.",
                11,
                Typeface.NORMAL,
                COLOR_MUTED);
        footer.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams footerParams = matchWrap();
        footerParams.topMargin = dp(10);
        root.addView(footer, footerParams);

        return scroll;
    }

    private View createHeader() {
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);

        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.ic_dopecam_mark);
        logo.setContentDescription("DopeCam");
        logo.setScaleType(ImageView.ScaleType.FIT_CENTER);
        header.addView(logo, new LinearLayout.LayoutParams(dp(36), dp(36)));

        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);

        LinearLayout.LayoutParams titlesParams = new LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f);
        titlesParams.leftMargin = dp(10);
        header.addView(titles, titlesParams);

        TextView title = text("DopeCam", 22, Typeface.BOLD, COLOR_TEXT);
        titles.addView(title, matchWrap());

        TextView subtitle = text(
                "Wireless camera bridge",
                12,
                Typeface.NORMAL,
                COLOR_SECONDARY);
        titles.addView(subtitle, matchWrap());

        return header;
    }

    private TextView sectionLabel(String value) {
        TextView label = text(value, 10, Typeface.BOLD, COLOR_SECONDARY);
        label.setLetterSpacing(0.09f);
        return label;
    }

    private View divider() {
        View divider = new View(this);
        divider.setBackgroundColor(COLOR_DIVIDER);
        return divider;
    }

    private LinearLayout.LayoutParams dividerParams(int topMarginDp, int bottomMarginDp) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(1));
        params.topMargin = dp(topMarginDp);
        params.bottomMargin = dp(bottomMarginDp);
        return params;
    }

    private LinearLayout.LayoutParams sectionParams(int topMarginDp) {
        LinearLayout.LayoutParams params = matchWrap();
        params.topMargin = dp(topMarginDp);
        params.bottomMargin = dp(7);
        return params;
    }

    private TextView text(String value, int sizeSp, int style, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sizeSp);
        view.setTextColor(color);
        view.setTypeface(Typeface.DEFAULT, style);
        view.setGravity(Gravity.START);
        return view;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private RippleDrawable buttonBackground(
            int normalColor,
            int pressedColor,
            int strokeColor) {
        GradientDrawable content = new GradientDrawable();
        content.setColor(normalColor);
        content.setCornerRadius(dp(10));

        if (strokeColor != Color.TRANSPARENT) {
            content.setStroke(dp(1), strokeColor);
        }

        return new RippleDrawable(
                ColorStateList.valueOf(pressedColor),
                content,
                null);
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
            setState("\u25CF Ready for PC", COLOR_READY, null);
        } else if (armed) {
            setState(
                    "\u25CF Starting\u2026",
                    COLOR_SECONDARY,
                    "Preparing camera and local services.");
        } else if (error != null && !error.isBlank()) {
            setState("\u25CF Service error", COLOR_ERROR, error);
        } else {
            setState("\u25CF Not armed", COLOR_MUTED, null);
        }

        updateArmButton();

        currentIp = LanAddress.wifiIpv4(this);
        boolean hasIp = currentIp != null && !currentIp.isBlank();

        if (hasIp) {
            ipValue.setText(currentIp);
            ipValue.setTextColor(COLOR_TEXT);
            networkMeta.setText(
                    "Control " + DiscoveryServer.CONTROL_PORT
                            + " \u00B7 local network");
            networkHint.setText(
                    "Discover from Windows, or connect manually with this address.");
        } else {
            ipValue.setText("No Wi-Fi IPv4");
            ipValue.setTextColor(COLOR_MUTED);
            networkMeta.setText("Control " + DiscoveryServer.CONTROL_PORT);
            networkHint.setText(
                    "Connect this phone and the PC to the same Wi-Fi network.");
        }

        copyAction.setEnabled(hasIp);
        copyAction.setAlpha(hasIp ? 1.0f : 0.35f);
    }

    private void setState(String title, int color, String detail) {
        state.setText(title);
        state.setTextColor(color);

        if (detail == null || detail.isBlank()) {
            stateDetail.setText("");
            stateDetail.setVisibility(View.GONE);
        } else {
            stateDetail.setText(detail);
            stateDetail.setVisibility(View.VISIBLE);
        }
    }

    private void updateArmButton() {
        if (armed) {
            armButton.setText("Disarm");
            armButton.setTextColor(COLOR_TEXT);
            armButton.setBackground(buttonBackground(
                    COLOR_BACKGROUND,
                    COLOR_PRESS,
                    COLOR_PRIMARY));
        } else {
            armButton.setText("Arm DopeCam");
            armButton.setTextColor(Color.WHITE);
            armButton.setBackground(buttonBackground(
                    COLOR_PRIMARY,
                    COLOR_PRIMARY_PRESSED,
                    Color.TRANSPARENT));
        }
    }

    private void copyIp() {
        if (currentIp == null || currentIp.isBlank()) {
            return;
        }

        ClipboardManager clipboard =
                (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);

        clipboard.setPrimaryClip(
                ClipData.newPlainText("DopeCam phone IPv4", currentIp));

        Toast.makeText(this, "IPv4 copied", Toast.LENGTH_SHORT).show();
    }

    private void ensurePermissionsAndArm() {
        boolean camera =
                checkSelfPermission(Manifest.permission.CAMERA)
                        == PackageManager.PERMISSION_GRANTED;
        boolean notifications =
                checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                        == PackageManager.PERMISSION_GRANTED;

        if (camera && notifications) {
            arm();
            return;
        }

        requestPermissions(
                new String[] {
                        Manifest.permission.CAMERA,
                        Manifest.permission.POST_NOTIFICATIONS
                },
                REQUEST_PERMISSIONS);
    }

    private void arm() {
        Intent intent = new Intent(this, DopecamService.class);
        startForegroundService(intent);

        setState(
                "\u25CF Starting\u2026",
                COLOR_SECONDARY,
                "Preparing camera and local services.");

        refreshState();
    }

    private void disarm() {
        stopService(new Intent(this, DopecamService.class));
        armed = false;
        setState("\u25CF Not armed", COLOR_MUTED, null);
        updateArmButton();
    }

    @Override
    public void onRequestPermissionsResult(
            int requestCode,
            String[] permissions,
            int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        if (requestCode != REQUEST_PERMISSIONS) {
            return;
        }

        if (checkSelfPermission(Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED) {
            arm();
        } else {
            setState(
                    "\u25CF Camera permission required",
                    COLOR_ERROR,
                    "DopeCam cannot stream until camera access is allowed.");
        }
    }
}