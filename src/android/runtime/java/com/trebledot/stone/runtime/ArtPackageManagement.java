package com.trebledot.stone.runtime;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import org.json.JSONObject;

/** Standalone management: selected-image parser/verifier, no APK classes or Application. */
public final class ArtPackageManagement {
    private ArtPackageManagement() { }
    private static Field field(Class<?> type, String name) throws Exception {
        Field value = type.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static PackageManager parser() throws Exception {
        ArtServices.initializeFrameworkState();
        ArtServices.installRegistry(new java.util.HashMap<>());
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> threadConstructor = threadType.getDeclaredConstructor(); threadConstructor.setAccessible(true);
        Object thread = threadConstructor.newInstance();
        field(threadType, "sCurrentActivityThread").set(null, thread);
        Class<?> managerType = Class.forName("android.content.pm.IPackageManager");
        android.os.Binder binder = ArtServices.service(managerType.getName(), (instance, method, arguments) -> {
            if (method.getName().equals("hasSystemFeature")) return false;
            throw new UnsupportedOperationException("Management parser service: " + method.getName());
        });
        Object service = binder.queryLocalInterface(managerType.getName());
        field(threadType, "sPackageManager").set(null, service);
        Class<?> implementation = Class.forName("android.app.ApplicationPackageManager");
        Constructor<?> constructor = implementation.getDeclaredConstructor(Class.forName("android.app.ContextImpl"), managerType);
        constructor.setAccessible(true);
        return (PackageManager) constructor.newInstance(null, service);
    }

    private static void result(String status, String name, String message) throws Exception {
        result(status, name, message, null);
    }
    private static void result(String status, String name, String message, Throwable error) throws Exception {
        JSONObject result = new JSONObject().put("format", 1).put("status", status)
            .put("packageName", name == null ? "" : name).put("message", message);
        ArtPackageCompatibility.Incompatible failure = ArtPackageCompatibility.failure(error);
        if (failure != null) {
            if (failure.reason.length() > 64 || failure.required.length() > 4096 || failure.available.length() > 4096)
                throw new java.io.IOException("APK compatibility details exceed the package result limits");
            result.put("compatibility", new JSONObject().put("reason", failure.reason)
                .put("required", failure.required).put("available", failure.available));
        }
        String line = "STONE_PACKAGE_RESULT " + result;
        if (line.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 65536)
            throw new java.io.IOException("Package result exceeds its transport limit");
        System.out.println(line);
    }
    private static void catalog(ArtPackageStore store) throws Exception {
        for (int attempt = 0; attempt < 8; attempt++) if (store.exportCatalog()) return;
        throw new java.io.IOException("Package list changed repeatedly; refresh to read its latest state");
    }
    private static String label(PackageInfo info) {
        try { return ArtArchive.labelInLocale(info, java.util.Locale.getDefault().toLanguageTag()); }
        catch (java.io.IOException error) { return info.packageName; }
    }

    public static int run(String operation, String target) throws Exception {
        ArtConfiguration.initialize();
        ArtPackageStore store = new ArtPackageStore(null);
        String name = "";
        boolean completed = false;
        try {
            // Finish only already-persisted removals; no new operation is approved here.
            for (PackageInfo pending : store.pendingRemovals())
                store.uninstall(pending.packageName, pending.applicationInfo.sourceDir);
            if (operation.equals("list")) {
                catalog(store); result("success", "", ""); return 0;
            }
            if (!ArtSettingsConfirmation.enabled()) throw new java.io.IOException("Native settings confirmation is unavailable");
            if (operation.equals("install")) {
                try (ArtPackageStore.Candidate candidate = store.prepare(parser(), target, null)) {
                    PackageInfo info = candidate.info; name = info.packageName;
                    String label = label(info);
                    String previousVersion = ArtSettingsConfirmation.version(store.get(name,0));
                    if (!ArtSettingsConfirmation.confirmBlocking("install",label,name,ArtSettingsConfirmation.version(info),previousVersion,ArtAppearance.settingsIcon(info),ArtSettingsConfirmation.permissions(info))) {
                        result("cancelled", name, ""); return 3;
                    }
                    store.commit(candidate, "com.trebledot.stone.settings");
                    completed = true;
                }
            } else if (operation.equals("uninstall")) {
                PackageInfo info = store.get(target, PackageManager.GET_SIGNING_CERTIFICATES); name = target;
                if (info == null) throw new java.io.IOException("Application is not installed");
                String label = label(info);
                if (!ArtSettingsConfirmation.confirmBlocking("uninstall", label, name, info.versionName,null,ArtAppearance.settingsIcon(info))) {
                    result("cancelled", name, ""); return 3;
                }
                store.uninstall(name, info.applicationInfo.sourceDir);
                completed = true;
            } else throw new IllegalArgumentException("Unsupported package operation");
            catalog(store);
            result("success", name, ""); return 0;
        } catch (Exception error) {
            android.util.Log.w("StonePackageManagement", "Package operation failed", error);
            try { store.exportCatalog(); } catch (Exception ignored) { }
            if (completed) { result("success", name, ArtText.get(ArtStrings.package_refresh_warning)); return 0; }
            result("failed", name, error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage(), error);
            return 1;
        }
    }
}
