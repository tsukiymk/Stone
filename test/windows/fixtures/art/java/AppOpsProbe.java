package com.trebledot.stone.probe;

import android.app.AppOpsManager;
import android.content.Context;
import android.provider.Settings;

/** Read-only framework checks must report denial without granting host settings access. */
final class AppOpsProbe {
    static void verify(Context context) {
        AppOpsManager manager = context.getSystemService(AppOpsManager.class);
        int uid = context.getApplicationInfo().uid;
        String name = context.getPackageName();
        if (manager == null || Settings.System.canWrite(context)
                || manager.unsafeCheckOpRawNoThrow(AppOpsManager.OPSTR_WRITE_SETTINGS,uid,name) != AppOpsManager.MODE_IGNORED
                || manager.unsafeCheckOpRawNoThrow(AppOpsManager.OPSTR_WRITE_SETTINGS,uid+1,name) != AppOpsManager.MODE_ERRORED
                || manager.unsafeCheckOpRawNoThrow(AppOpsManager.OPSTR_WRITE_SETTINGS,uid,"com.trebledot.stone.spoofed") != AppOpsManager.MODE_ERRORED)
            throw new AssertionError("Write-settings capability query granted access or lost caller identity");
        System.out.println("PASS image AppOps write-settings query: denied without exception, no host grant, foreign UID/package rejected.");
        if (android.os.Environment.isExternalStorageManager()
                || manager.unsafeCheckOpRawNoThrow("android:manage_external_storage",uid,name) != AppOpsManager.MODE_IGNORED
                || manager.unsafeCheckOpRawNoThrow("android:manage_external_storage",uid+1,name) != AppOpsManager.MODE_ERRORED
                || manager.unsafeCheckOpRawNoThrow("android:manage_external_storage",uid,"com.trebledot.stone.spoofed") != AppOpsManager.MODE_ERRORED)
            throw new AssertionError("All-files query granted unsupported access or lost caller identity");
        System.out.println("PASS image Environment/AppOps all-files query: denied without exception, foreign UID/package rejected.");
    }
}
