package com.trebledot.stone.cit;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.KeyEvent;
import android.webkit.WebView;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import com.trebledot.stone.cit.components.ComponentActivity;
import com.trebledot.stone.cit.documents.DocumentsActivity;
import com.trebledot.stone.cit.gles.GlesActivity;
import com.trebledot.stone.cit.nativeapp.NativeActivity;
import com.trebledot.stone.cit.nativejit.JitActivity;
import com.trebledot.stone.cit.notes.NotesActivity;
import com.trebledot.stone.cit.webview.WebActivity;
import java.util.Arrays;
import java.util.Locale;

import static com.trebledot.stone.cit.CitResult.State.*;

public final class CitActivity extends Activity {
    private LinearLayout content;
    private CitResult jni;
    private CitResult provider;
    private HardwareChecks hardware;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(16), dp(12), dp(16), dp(20));
        content.setBackgroundColor(0xfff3f6fb);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(content);
        heading(getString(R.string.app_name), 28);
        CitResult system = addResult("Android system", "system");
        showSystem(system);
        jni = addResult("JNI", "jni");
        provider = addResult("Image WebView provider", "webview-provider");
        addButton("Test WebView rendering and JavaScript", () -> open(WebActivity.class));
        addButton("Test multitouch, mouse and keyboard", () -> open(InputActivity.class));
        hardware = new HardwareChecks(this);
        hardware.addControls();
        heading("Framework checks", 20);
        addButton("OpenGL ES and Canvas", () -> open(GlesActivity.class));
        addButton("Android components", () -> open(ComponentActivity.class));
        addButton("SQLite and persistence", () -> open(NotesActivity.class));
        addButton("Document picker", () -> open(DocumentsActivity.class));
        setContentView(scroll);
        verifyJni();
        showWebViewProvider();
        verifyNestedMainLoop();
        Log.i("StoneCIT", "dashboard READY");
    }

    private static void verifyNestedMainLoop() {
        Handler handler = new Handler(Looper.getMainLooper());
        handler.post(() -> {
            RuntimeException exit = new RuntimeException("Exit nested main loop");
            handler.post(() -> { throw exit; });
            try {
                Looper.loop();
                Log.e("StoneCIT", "nested-main-loop FAIL returned before dispatch");
            } catch (RuntimeException error) {
                if (error != exit) throw error;
                Log.i("StoneCIT", "nested-main-loop PASS");
            }
        });
    }

    private void showSystem(CitResult result) {
        DisplayMetrics display = getResources().getDisplayMetrics();
        Configuration config = getResources().getConfiguration();
        PackageManager packages = getPackageManager();
        result.set(INFO, "Android " + Build.VERSION.RELEASE + " · API " + Build.VERSION.SDK_INT
                + "\n" + Build.MANUFACTURER + " " + Build.MODEL + " · " + Build.DEVICE
                + "\nABI " + Arrays.toString(Build.SUPPORTED_ABIS)
                + "\n" + display.widthPixels + " × " + display.heightPixels + " · " + display.densityDpi + " dpi"
                + "\nLocale " + (Build.VERSION.SDK_INT >= 24 ? config.getLocales().toLanguageTags() : config.locale)
                + "\nBuild " + Build.FINGERPRINT
                + "\nDeclared features · " + String.format(Locale.ROOT,
                        "camera=%b, mic=%b, audio=%b, Wi-Fi=%b, Bluetooth=%b, multitouch=%b",
                        packages.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY),
                        packages.hasSystemFeature(PackageManager.FEATURE_MICROPHONE),
                        packages.hasSystemFeature(PackageManager.FEATURE_AUDIO_OUTPUT),
                        packages.hasSystemFeature(PackageManager.FEATURE_WIFI),
                        packages.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH),
                        packages.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH)));
    }

    private void verifyJni() {
        if (!CitBuild.NATIVE_BUNDLED) {
            jni.set(UNAVAILABLE, "Built without the Android JNI libraries");
            return;
        }
        try {
            String abi = Build.SUPPORTED_ABIS[0];
            switch (abi) {
                case "x86_64": jni.set(PASS, NativeActivity.verify(this)); break;
                case "arm64-v8a": jni.set(PASS, JitActivity.verify()); break;
                default: jni.set(UNAVAILABLE, "No CIT JNI library for " + abi);
            }
        } catch (RuntimeException | LinkageError | AssertionError error) {
            jni.error(error);
        }
    }

    private void showWebViewProvider() {
        try {
            PackageInfo info = Build.VERSION.SDK_INT >= 26 ? WebView.getCurrentWebViewPackage() : null;
            if (info == null) provider.set(UNAVAILABLE, "Android did not report a WebView provider");
            else provider.set(INFO, info.packageName + " " + info.versionName);
        } catch (RuntimeException | LinkageError error) {
            provider.error(error);
        }
    }

    void heading(String title, int size) {
        TextView label = new TextView(this);
        label.setText(title);
        label.setTextSize(size);
        label.setTextColor(0xff18283e);
        label.setPadding(0, dp(12), 0, dp(4));
        content.addView(label);
    }

    CitResult addResult(String title, String id) {
        heading(title, 18);
        CitResult result = new CitResult(this, id);
        content.addView(result.view());
        return result;
    }

    Button addButton(String title, Runnable action) {
        Button button = new Button(this);
        button.setText(title);
        button.setAllCaps(false);
        button.setOnClickListener(view -> action.run());
        content.addView(button);
        return button;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void open(Class<? extends Activity> screen) {
        startActivity(new Intent(this, screen));
    }

    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (event.getRepeatCount() == 0) {
            switch (keyCode) {
                case KeyEvent.KEYCODE_I: open(InputActivity.class); return true;
                case KeyEvent.KEYCODE_W: open(WebActivity.class); return true;
                case KeyEvent.KEYCODE_C: open(ComponentActivity.class); return true;
                case KeyEvent.KEYCODE_N: open(NotesActivity.class); return true;
                case KeyEvent.KEYCODE_G: open(GlesActivity.class); return true;
                default: break;
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override public void onRequestPermissionsResult(int code, String[] names, int[] grants) {
        super.onRequestPermissionsResult(code, names, grants);
        if (hardware != null) hardware.permissionsReturned(code);
    }

    @Override protected void onPause() {
        if (hardware != null) hardware.pause();
        super.onPause();
    }
}
