package com.trebledot.stone.runtime;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;
import org.json.JSONObject;

/** One native settings request, retained independently of Activity recreation. */
public final class ArtSettingsConfirmation {
    private ArtSettingsConfirmation() { }
    public static boolean enabled() { return enabledNative(); }
    private static native boolean enabledNative();
    private static native long createNative(byte[] request) throws java.io.IOException;
    private static native boolean awaitNative(long request);
    private static native void cancelNative(long request);

    static Session create(String operation, String name, String packageName, String version, String installer, boolean verified)
            throws Exception {
        return create(operation,name,packageName,version,installer,verified,null);
    }
    static Session create(String operation, String name, String packageName, String version, String installer, boolean verified, String previousVersion)
            throws Exception {
        return create(operation,name,packageName,version,installer,verified,previousVersion,new byte[0]);
    }
    static Session create(String operation, String name, String packageName, String version, String installer, boolean verified, String previousVersion, byte[] icon)
            throws Exception {
        return create(operation,name,packageName,version,installer,verified,previousVersion,icon,null);
    }
    static Session create(String operation, String name, String packageName, String version, String installer, boolean verified, String previousVersion, byte[] icon, String[] permissions)
            throws Exception {
        return new Session(request(operation,name,packageName,version,installer,verified,previousVersion,icon,permissions));
    }

    static String version(android.content.pm.PackageInfo info) {
        return info == null ? null : info.versionName == null || info.versionName.isEmpty() ? Long.toString(info.getLongVersionCode()) : info.versionName;
    }
    static String[] permissions(android.content.pm.PackageInfo info) {
        return info.requestedPermissions == null ? new String[0] : info.requestedPermissions.clone();
    }
    private static long request(String operation, String name, String packageName, String version, String installer, boolean verified, String previousVersion, byte[] icon, String[] permissions)
            throws Exception {
        if (icon.length > 32768) throw new IllegalArgumentException("Settings icon exceeds its size limit");
        JSONObject json = new JSONObject().put("operation", operation).put("name", name).put("packageName", packageName)
            .put("version", version == null ? "" : version).put("installer", installer == null ? "" : installer).put("verified", verified)
            .put("previousVersion",previousVersion == null ? "" : previousVersion)
            .put("iconPng",android.util.Base64.encodeToString(icon,android.util.Base64.NO_WRAP))
            .put("requestedPermissions",permissions == null ? JSONObject.NULL : new org.json.JSONArray(java.util.Arrays.asList(permissions)));
        return createNative(json.toString().getBytes(StandardCharsets.UTF_8));
    }

    // Only the separate management process waits synchronously. APK Activities use Session.
    static boolean confirmBlocking(String operation, String name, String packageName, String version) throws Exception {
        return confirmBlocking(operation,name,packageName,version,null);
    }
    static boolean confirmBlocking(String operation, String name, String packageName, String version, String previousVersion) throws Exception {
        return confirmBlocking(operation,name,packageName,version,previousVersion,new byte[0]);
    }
    static boolean confirmBlocking(String operation, String name, String packageName, String version, String previousVersion, byte[] icon) throws Exception {
        return confirmBlocking(operation,name,packageName,version,previousVersion,icon,null);
    }
    static boolean confirmBlocking(String operation, String name, String packageName, String version, String previousVersion, byte[] icon, String[] permissions) throws Exception {
        return awaitNative(request(operation,name,packageName,version,ArtText.get(ArtStrings.settings_name),true,previousVersion,icon,permissions));
    }

    static final class Session {
        private final Handler main = new Handler(Looper.getMainLooper());
        private final long handle;
        private Consumer<Boolean> callback;
        private Boolean result;
        private boolean cancelled;
        Session(long handle) {
            if (handle == 0) throw new IllegalStateException("Missing native confirmation request");
            this.handle = handle;
            Thread worker = new Thread(() -> {
                boolean accepted = false;
                try { accepted = awaitNative(handle); }
                catch (RuntimeException error) { Log.w("StoneSettings", "Native confirmation failed", error); }
                final boolean decision = accepted;
                main.post(() -> { result = !cancelled && decision; deliver(); });
            }, "Stone settings confirmation");
            worker.setDaemon(true);
            try { worker.start(); }
            catch (RuntimeException | Error error) { cancelNative(handle); awaitNative(handle); throw error; }
        }
        void attach(Consumer<Boolean> owner) { callback = owner; main.post(this::deliver); }
        void detach() { callback = null; }
        void cancel() { cancelled = true; cancelNative(handle); callback = null; }
        private void deliver() { if (callback != null && result != null && !cancelled) callback.accept(result); }
    }
}
