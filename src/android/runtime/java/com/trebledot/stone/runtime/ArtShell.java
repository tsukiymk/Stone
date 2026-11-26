package com.trebledot.stone.runtime;

import android.content.pm.PackageInfo;
import android.util.Log;

/** Per-user Windows entry points. Shell failures cannot undo an APK transaction. */
final class ArtShell {
    private ArtShell() { }
    private static native boolean enabled();
    private static native void update(String name, String label, boolean installed);
    private static native String[] icon(String name, String label, byte[] png);
    static String[] appearance(String name, String label, byte[] png) {
        return enabled() ? icon(name,label,png) : new String[]{"",""};
    }
    static void installed(PackageInfo info, boolean launchable) {
        if (!enabled()) return;
        try {
            String label = info.packageName;
            if (launchable) try { label = ArtArchive.labelInLocale(info, java.util.Locale.getDefault().toLanguageTag()); }
            catch (java.io.IOException error) { Log.w("StoneShell", "Cannot read application label", error); }
            update(info.packageName, label, launchable);
            if (launchable) {
                android.content.pm.ActivityInfo launcher = launcher(info);
                if (launcher != null) ArtArchive.withResources(info,java.util.Locale.getDefault().toLanguageTag(),resources -> {
                    appearance(info.packageName,ArtAppearance.label(resources,launcher),ArtAppearance.render(resources,launcher.getIconResource()));
                    return null;
                });
            }
        } catch (Exception error) { Log.w("StoneShell", "APK installed, but its Windows shortcut could not be updated", error); }
    }
    private static android.content.pm.ActivityInfo launcher(PackageInfo info) throws java.io.IOException {
        java.util.Map<String,android.content.pm.ComponentInfo> components = new java.util.HashMap<>();
        if (info.activities == null) return null;
        for (android.content.pm.ActivityInfo activity : info.activities) components.put(activity.name,activity);
        ArtComponentSettings settings = new ArtComponentSettings(info.packageName,new java.io.File(info.applicationInfo.dataDir),components);
        for (String entry : ArtPackage.launcherNames(info)) {
            android.content.pm.ActivityInfo activity = (android.content.pm.ActivityInfo) components.get(entry.split("\t",2)[0]);
            if (activity == null || !settings.enabled(activity)) continue;
            if (activity.targetActivity != null) {
                android.content.pm.ComponentInfo target = components.get(activity.targetActivity);
                if (target == null || !settings.enabled(target)) continue;
            }
            return activity;
        }
        return null;
    }
    static void removed(String name) {
        if (!enabled()) return;
        try { update(name, "", false); }
        catch (Exception error) { Log.w("StoneShell", "APK removed, but its Windows shortcut could not be removed", error); }
    }
}
