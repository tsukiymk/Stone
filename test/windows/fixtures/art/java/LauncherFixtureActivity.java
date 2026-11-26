package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Properties;

/** Installed APK fixture; no dependency on Stone's Java runtime classes. */
public final class LauncherFixtureActivity extends Activity {
    public static final class App extends Application {
        @Override public void onCreate() {
            super.onCreate();
            try { verify(this); }
            catch (Exception error) { throw new IllegalStateException("Installed Application identity", error); }
        }
    }

    private static Properties verify(android.content.Context context) throws Exception {
        Properties expected = new Properties();
        try (FileInputStream input = new FileInputStream("/data/installed-launch-expected.properties")) { expected.load(input); }
        int uid = Integer.parseInt(expected.getProperty("uid"));
        if (android.os.Process.myUid() != uid || android.os.Binder.getCallingUid() != uid
                || android.system.Os.getuid() != uid || context.getApplicationInfo().uid != uid
                || context.getPackageManager().getPackageUid(context.getPackageName(), 0) != uid
                || !context.getPackageCodePath().equals(expected.getProperty("source"))
                || !context.getApplicationInfo().dataDir.equals(expected.getProperty("data"))
                || !new File(context.getFilesDir(), "installed-sentinel.txt").isFile())
            throw new AssertionError("Installed UID, source or data identity mismatch: Process=" + android.os.Process.myUid()
                + " Binder=" + android.os.Binder.getCallingUid() + " app=" + context.getApplicationInfo().uid + " expected=" + uid);
        return expected;
    }

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        try {
            Properties result = verify(this);
            android.content.SharedPreferences preferences = getSharedPreferences("launches", MODE_PRIVATE);
            int count = preferences.getInt("count", 0) + 1;
            if (!preferences.edit().putInt("count", count).commit()) throw new AssertionError("Installed launch persistence");
            result.setProperty("count", Integer.toString(count));
            result.setProperty("pid", Integer.toString(android.os.Process.myPid()));
            result.setProperty("component", getComponentName().flattenToString());
            try (FileOutputStream output = new FileOutputStream("/data/installed-launch-result.properties")) {
                result.store(output, "Installed APK identity and data"); output.getFD().sync();
            }
            android.widget.TextView text = new android.widget.TextView(this);
            text.setText("Installed application\nUID " + android.os.Process.myUid() + "\nLaunch " + count);
            text.setTextSize(24); text.setPadding(24, 24, 24, 24); setContentView(text);
            System.out.println("ART installed Application and Activity identity passed; UID=" + android.os.Process.myUid() + " launch=" + count);
        } catch (Exception error) { throw new IllegalStateException("Installed Activity identity", error); }
    }
}
