package com.trebledot.stone.probe;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;

/** Resolution/identity checks only: this probe never activates the Windows browser. */
final class WebLinksProbe {
    static void verify(Context context) throws Exception {
        PackageManager packages = context.getPackageManager();
        for (String url : new String[]{"https://github.com/al54/Bad-Pixels/issues", "http://example.com:8080/path?q=hello%20world#part", "HTTPS://example.com/", "https://example.com/中文"}) {
            Intent intent = new Intent(Intent.ACTION_VIEW,Uri.parse(url)).addCategory(Intent.CATEGORY_BROWSABLE);
            ComponentName component = intent.resolveActivity(packages);
            if (component == null || !"com.trebledot.stone.browser".equals(component.getPackageName())) throw new AssertionError("Windows browser missing from web resolution: " + url);
            ActivityInfo info = packages.getActivityInfo(component,0);
            if (!info.enabled || !info.exported || !component.getClassName().equals(info.name)) throw new AssertionError("Browser component identity mismatch");
            if (new Intent(intent).setComponent(component).resolveActivityInfo(packages,0) == null) throw new AssertionError("Explicit resolved browser disappeared");
        }
        for (String url : new String[]{"file:///sdcard/test.apk", "javascript:alert(1)", "ms-settings:appsfeatures", "https:example.com", "https:///example.com",
                "https://user:pass@example.com/", "https://@example.com/", "https://example.com/a b", "https://example.com/\nnext", "https://example.com/\\path", "https://example.com/\"quoted"}) {
            if (new Intent(Intent.ACTION_VIEW,Uri.parse(url)).resolveActivity(packages) != null) throw new AssertionError("Non-web or malformed URI resolved to browser: " + url);
        }
        Intent web = new Intent(Intent.ACTION_VIEW,Uri.parse("https://example.com/"));
        for (Intent invalid : new Intent[]{new Intent(web).setPackage("com.some.other.browser"), new Intent(web).setDataAndType(web.getData(),"text/plain"),
                new Intent(web).setAction(Intent.ACTION_SEND), new Intent(web).putExtra("unhandled","payload"), new Intent(web).addCategory(Intent.CATEGORY_LAUNCHER)})
            if (invalid.resolveActivity(packages) != null) throw new AssertionError("Unsupported web Intent payload or target silently discarded");
        System.out.println("Windows web link resolution, explicit component and unsupported URI/payload checks passed; no browser activated.");
    }
}
