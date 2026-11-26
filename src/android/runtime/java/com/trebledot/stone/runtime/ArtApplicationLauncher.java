package com.trebledot.stone.runtime;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import java.io.IOException;

/** Dispatches installed MAIN/LAUNCHER entries to a separate Windows ART process. */
final class ArtApplicationLauncher {
    private static native void launchNative(String source, String packageName) throws IOException;

    static int start(ArtApplication application, Intent intent, int request) {
        if (intent == null || intent.getComponent() == null) return -91;
        PackageManager manager = application.context.getPackageManager();
        String name = intent.getComponent().getPackageName();
        Intent launcher = manager.getLaunchIntentForPackage(name);
        if (launcher == null || !intent.getComponent().equals(launcher.getComponent())) return -92;
        try {
            ActivityInfo info = manager.getActivityInfo(intent.getComponent(), 0);
            if (!info.exported || info.permission != null) throw new SecurityException("Launcher is not accessible to the calling application");
            if (request >= 0 || !Intent.ACTION_MAIN.equals(intent.getAction()) || !intent.hasCategory(Intent.CATEGORY_LAUNCHER)
                    || intent.getData() != null || intent.getType() != null || intent.getSelector() != null
                    || intent.getClipData() != null || intent.getExtras() != null)
                throw new UnsupportedOperationException("Cross-process Activity results and non-launcher Intent payloads are not connected yet");
            for (String category : intent.getCategories())
                if (!Intent.CATEGORY_LAUNCHER.equals(category) && !Intent.CATEGORY_DEFAULT.equals(category))
                    throw new UnsupportedOperationException("Unsupported cross-process launcher category: " + category);
            launchNative(info.applicationInfo.sourceDir, name);
            return 0;
        } catch (PackageManager.NameNotFoundException missing) { return -92; }
        catch (IOException error) { throw new IllegalStateException("Could not start installed application " + name, error); }
    }
}
