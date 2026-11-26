package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import java.util.function.Consumer;

/** Standard Activity.requestPermissions results backed by native notification consent. */
public final class ArtPermissionActivity extends Activity {
    private Request request;
    private boolean finished;

    @Override protected void onCreate(Bundle state) {
        ArtSettingsUi.prepare(this);
        super.onCreate(state);
        Object retained = getLastNonConfigurationInstance();
        request = retained instanceof Request ? (Request) retained : new Request(getIntent(),
            getApplicationInfo().packageName, getApplicationInfo().loadLabel(getPackageManager()).toString());
        ArtSettingsUi ui = new ArtSettingsUi(this,ArtStrings.notification_title);
        ui.application(getApplicationInfo().loadLabel(getPackageManager()),getPackageName(),null);
        ui.waiting(ArtText.get(this,ArtStrings.notification_wait));
        ui.show();
        request.attach(result -> {
            if (finished) return;
            finished = true;
            setResult(RESULT_OK, new Intent().putExtra(ArtPermissions.NAMES, request.permissions).putExtra(ArtPermissions.RESULTS, result));
            finish();
        });
    }
    @Override public void onBackPressed() { if (request != null) request.deny(); }
    @Override public Object onRetainNonConfigurationInstance() { return request; }
    @Override protected void onDestroy() {
        if (request != null) { request.callback = null; if (!isChangingConfigurations()) request.close(); }
        super.onDestroy();
    }

    private static final class Request {
        final String[] permissions;
        final Handler main = new Handler(Looper.getMainLooper());
        Consumer<int[]> callback;
        ArtSettingsConfirmation.Session confirmation;
        int[] result;
        boolean deciding, closed;
        Request(Intent intent, String packageName, String label) {
            String[] names = intent.getStringArrayExtra(ArtPermissions.NAMES);
            if (names == null || names.length > 64) names = new String[0];
            for (String name : names) if (name == null || name.length() > 256) { names = new String[0]; break; }
            permissions = names.clone();
            boolean notification = java.util.Arrays.asList(permissions).contains(ArtPermissions.NOTIFICATIONS)
                && ArtServices.notifications.requestable();
            if (!notification || ArtServices.notifications.consented()) { complete(); return; }
            try {
                if (!ArtSettingsConfirmation.enabled()) { complete(); return; }
                confirmation = ArtSettingsConfirmation.create("notifications", label, packageName, "", "", true);
                confirmation.attach(accepted -> { if (accepted) allow(); else deny(); });
            } catch (Exception error) { android.util.Log.w("StonePermissions", "Cannot request notification consent", error); complete(); }
        }
        void attach(Consumer<int[]> owner) { callback = owner; main.post(this::deliver); }
        void allow() {
            if (closed || deciding || result != null) return;
            deciding = true;
            ArtServices.notifications.allow(() -> { if (!closed) complete(); });
        }
        void deny() { if (deciding || result != null) return; if (confirmation != null) confirmation.cancel(); complete(); }
        void complete() {
            if (closed || result != null) return;
            result = new int[permissions.length];
            for (int index = 0; index < permissions.length; ++index) result[index] = ArtPermissions.NOTIFICATIONS.equals(permissions[index])
                && ArtServices.notifications.granted() ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
            main.post(this::deliver);
        }
        void deliver() { if (!closed && callback != null && result != null) callback.accept(result.clone()); }
        void close() { closed = true; if (confirmation != null) confirmation.cancel(); }
    }
}
