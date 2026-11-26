package com.trebledot.stone.runtime;

import android.os.Build;
import android.util.Xml;
import java.io.File;
import java.io.FileInputStream;
import java.lang.reflect.Constructor;
import java.util.ArrayList;
import java.util.List;
import org.xmlpull.v1.XmlPullParser;

/** Image permission split rules and the host-owned notification consent entry. */
final class ArtPermissions {
    static final String NOTIFICATIONS = "android.permission.POST_NOTIFICATIONS";
    static final String ACTION = "android.content.pm.action.REQUEST_PERMISSIONS";
    static final String NAMES = "android.content.pm.extra.REQUEST_PERMISSIONS_NAMES";
    static final String RESULTS = "android.content.pm.extra.REQUEST_PERMISSIONS_RESULTS";
    static boolean requestable(ArtPackage apk, String permission) {
        return NOTIFICATIONS.equals(permission) && apk.packageInfo.requestedPermissions != null
            && java.util.Arrays.asList(apk.packageInfo.requestedPermissions).contains(permission);
    }
    static android.content.pm.ActivityInfo info(ArtPackage apk, android.content.ComponentName component) {
        if (component == null || !apk.applicationInfo.packageName.equals(component.getPackageName())
                || !ArtPermissionActivity.class.getName().equals(component.getClassName())) return null;
        android.content.pm.ActivityInfo info = new android.content.pm.ActivityInfo(apk.activityInfo);
        info.name = ArtPermissionActivity.class.getName(); info.targetActivity = null;
        info.exported = false; info.configChanges = 0; info.labelRes = 0; info.nonLocalizedLabel = "Stone";
        info.theme = android.R.style.Theme_DeviceDefault;
        return info;
    }
    static android.content.pm.ActivityInfo resolve(ArtPackage apk, android.content.Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction()) || intent.getData() != null
                || intent.getPackage() != null && !apk.applicationInfo.packageName.equals(intent.getPackage())) return null;
        android.content.ComponentName component = intent.getComponent();
        return info(apk, component == null ? new android.content.ComponentName(apk.applicationInfo.packageName,
            ArtPermissionActivity.class.getName()) : component);
    }
    private List<Object> splits;

    synchronized List<Object> splitPermissions(boolean includeDisabled) throws Exception {
        Class<?> type = Class.forName("android.content.pm.permission.SplitPermissionInfoParcelable");
        Constructor<?> constructor = type.getConstructor(String.class, List.class, int.class, String.class, boolean.class);
        if (splits == null) {
            // Stone has no aconfigd-populated /metadata boot storage. Use the
            // selected image's flag protos through the image's own decoder.
            Class<?> flags = Class.forName("com.android.internal.pm.pkg.component.AconfigFlags");
            Object flagCache = flags.getMethod("getInstance").invoke(null);
            java.lang.reflect.Method loadFlags = flags.getDeclaredMethod("loadAconfigDefaultValues", byte[].class);
            loadFlags.setAccessible(true);
            List<?> protos = (List<?>) Class.forName("android.aconfig.DeviceProtos").getMethod("parsedFlagsProtoPaths").invoke(null);
            for (Object path : protos) {
                File file = new File((String) path);
                if (!file.isFile()) continue;
                try (FileInputStream input = new FileInputStream(file)) {
                    java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                    byte[] buffer = new byte[8192];
                    int count;
                    while ((count = input.read(buffer)) != -1) bytes.write(buffer, 0, count);
                    loadFlags.invoke(flagCache, (Object) bytes.toByteArray());
                }
            }
            List<Object> result = new ArrayList<>();
            for (String partition : new String[]{"/system", "/system_ext", "/product", "/vendor", "/odm"}) {
                File[] files = new File(partition + "/etc/permissions").listFiles();
                if (files == null) continue;
                java.util.Arrays.sort(files);
                for (File file : files) {
                    if (!file.isFile() || !file.getName().endsWith(".xml")) continue;
                    try (FileInputStream input = new FileInputStream(file)) {
                        XmlPullParser parser = Xml.newPullParser();
                        parser.setInput(input, "UTF-8");
                        while (parser.next() != XmlPullParser.END_DOCUMENT) {
                            if (parser.getEventType() != XmlPullParser.START_TAG || parser.getDepth() != 2
                                    || !"split-permission".equals(parser.getName())) continue;
                            String name = parser.getAttributeValue(null, "name");
                            String target = parser.getAttributeValue(null, "targetSdk");
                            String flag = parser.getAttributeValue(null, "featureFlag");
                            boolean negated = flag != null && flag.startsWith("!");
                            if (negated) flag = flag.substring(1);
                            List<String> added = new ArrayList<>();
                            while (parser.next() != XmlPullParser.END_DOCUMENT) {
                                if (parser.getEventType() == XmlPullParser.END_TAG && parser.getDepth() == 2) break;
                                if (parser.getEventType() == XmlPullParser.START_TAG && parser.getDepth() == 3
                                        && "new-permission".equals(parser.getName())) {
                                    String permission = parser.getAttributeValue(null, "name");
                                    if (permission != null && !permission.isEmpty()) added.add(permission);
                                }
                            }
                            if (name != null && !name.isEmpty() && !added.isEmpty())
                                result.add(constructor.newInstance(name, added,
                                    target == null ? Build.VERSION_CODES.CUR_DEVELOPMENT + 1 : Integer.parseInt(target), flag, negated));
                        }
                    }
                }
            }
            splits = result;
        }
        List<Object> result = new ArrayList<>();
        for (Object split : splits) {
            String flag = (String) type.getMethod("getFeatureFlag").invoke(split);
            if (!includeDisabled && flag != null) {
                Class<?> flags = Class.forName("com.android.internal.pm.pkg.component.AconfigFlags");
                Object cache = flags.getMethod("getInstance").invoke(null);
                boolean enabled = Boolean.TRUE.equals(flags.getMethod("getFlagValue", String.class).invoke(cache, flag));
                if (enabled == (Boolean) type.getMethod("isFeatureFlagNegated").invoke(split)) continue;
            }
            result.add(constructor.newInstance(type.getMethod("getSplitPermission").invoke(split),
                new ArrayList<>((List<?>) type.getMethod("getNewPermissions").invoke(split)),
                type.getMethod("getTargetSdk").invoke(split), flag, type.getMethod("isFeatureFlagNegated").invoke(split)));
        }
        return result;
    }
}
