package com.trebledot.stone.runtime;

import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.ServiceInfo;
import java.io.IOException;

/** Process identity is fixed before any application code runs. */
final class ArtProcess {
    private static final String requested = System.getProperty("stone.application.process", "");
    private static byte[] summary;
    static synchronized void stateSummary(byte[] state) {
        if(state!=null && state.length>128) throw new IllegalArgumentException("Process state summary exceeds 128 bytes");
        summary=state==null?null:state.clone();
    }
    static synchronized String diagnosticStateSummary() {
        return summary==null?"":android.util.Base64.encodeToString(summary,android.util.Base64.NO_WRAP);
    }
    static String current(ArtPackage apk) { return requested.isEmpty() ? apk.applicationInfo.processName : requested; }
    static String diagnosticName(String name) { return requested.isEmpty() ? name : name+"!"+requested.substring(requested.indexOf(':')+1); }
    static void validate(ApplicationInfo app, Iterable<ComponentInfo> components) throws IOException {
        if (requested.isEmpty()) return;
        if (!requested.matches(java.util.regex.Pattern.quote(app.packageName) + ":[A-Za-z0-9_]+") || requested.length() > 255)
            throw new IOException("Invalid private application process");
        for (ComponentInfo info : components) if (requested.equals(info.processName)
                && (!(info instanceof ServiceInfo) || (((ServiceInfo)info).flags & ServiceInfo.FLAG_ISOLATED_PROCESS) == 0)) return;
        throw new IOException("Application process is not declared in the APK: " + requested);
    }
}
