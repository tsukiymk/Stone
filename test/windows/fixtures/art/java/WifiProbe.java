package com.trebledot.stone.probe;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.NetworkInfo;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import java.lang.reflect.Method;
import java.util.Arrays;

/** Public image APIs checked against the independently sampled Service cache. */
final class WifiProbe {
    static void verify(Context context) throws Exception {
        Method read = Class.forName("com.trebledot.stone.runtime.ArtWifi").getDeclaredMethod("queryHostState"); read.setAccessible(true);
        long deadline = android.os.SystemClock.uptimeMillis()+20000;
        long[] initial;
        do { initial = (long[])read.invoke(null); if (initial[8] > 0) break; Thread.sleep(20); }
        while (android.os.SystemClock.uptimeMillis() < deadline);
        if (initial[8] == 0) throw new AssertionError("No real WLAN sample");
        boolean present = initial[2] != 0;
        while (context.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WIFI) != present
                && android.os.SystemClock.uptimeMillis() < deadline) Thread.sleep(20);
        if (context.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WIFI) != present
                || context.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WIFI,1)
                || context.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WIFI_DIRECT))
            throw new AssertionError("Invented or missing WLAN hardware feature: index="+initial[2]+" samples="+initial[8]
                +" feature="+context.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WIFI)
                +" v1="+context.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WIFI,1));
        android.content.pm.FeatureInfo[] features = context.getPackageManager().getSystemAvailableFeatures();
        if (features.length != (present ? 1 : 0) || present && !android.content.pm.PackageManager.FEATURE_WIFI.equals(features[0].name))
            throw new AssertionError("Feature inventory disagrees with WLAN presence");
        WifiManager wifi = context.getSystemService(WifiManager.class);
        if ((wifi != null) != present) throw new AssertionError("WifiManager disagrees with sampled WLAN presence");
        if (!present) { System.out.println("PASS image Wi-Fi: sampled host has no WLAN adapter; framework manager is absent."); return; }
        String[] requested = context.getPackageManager().getPackageInfo(context.getPackageName(),0).requestedPermissions;
        java.util.List<String> permissions = requested == null ? java.util.Collections.emptyList() : Arrays.asList(requested);
        for (String name : new String[]{"android.permission.ACCESS_WIFI_STATE","android.permission.CHANGE_WIFI_MULTICAST_STATE"}) {
            int expected = permissions.contains(name) ? android.content.pm.PackageManager.PERMISSION_GRANTED : android.content.pm.PackageManager.PERMISSION_DENIED;
            if (context.checkSelfPermission(name) != expected) throw new AssertionError("Wi-Fi normal permission query disagrees with manifest");
        }
        if (!permissions.contains("android.permission.ACCESS_WIFI_STATE")) {
            try { wifi.getWifiState(); throw new AssertionError("Undeclared Wi-Fi read permission was granted"); }
            catch (SecurityException expected) { }
        } else {
            long[] cached = null; WifiInfo info = null;
            long end = android.os.SystemClock.uptimeMillis()+20000;
            do {
                cached = (long[])read.invoke(null); info = wifi.getConnectionInfo();
                if (cached.length == 9 && cached[8] > 0 && wifi.getWifiState() == cached[0]
                        && (info.getIpAddress()&0xffffffffL) == cached[3]) break;
                Thread.sleep(20);
            } while (android.os.SystemClock.uptimeMillis() < end);
            if (cached.length != 9 || cached[8] == 0 || wifi.getWifiState() != cached[0] || (info.getIpAddress()&0xffffffffL) != cached[3])
                throw new AssertionError("WifiManager does not reflect the real Service sample");
            android.net.DhcpInfo dhcp = wifi.getDhcpInfo();
            if (cached[1] != 0 && cached[3] != 0 && cached[4] != 0) {
                if (dhcp == null || (dhcp.ipAddress&0xffffffffL) != cached[3] || (dhcp.netmask&0xffffffffL) != cached[4]
                        || (dhcp.gateway&0xffffffffL) != cached[5] || (dhcp.dns1&0xffffffffL) != cached[6] || (dhcp.dns2&0xffffffffL) != cached[7])
                    throw new AssertionError("Wi-Fi address configuration changed across the framework boundary");
            } else if (dhcp != null) throw new AssertionError("Wi-Fi address configuration was invented");
            if (info.getBSSID() != null && !"02:00:00:00:00:00".equals(info.getBSSID())) throw new AssertionError("Unexpected uncollected BSSID");
            IntentFilter filter = new IntentFilter(WifiManager.NETWORK_STATE_CHANGED_ACTION);
            Intent first = context.registerReceiver(null,filter);
            NetworkInfo original = first == null ? null : first.getParcelableExtra(WifiManager.EXTRA_NETWORK_INFO,NetworkInfo.class);
            if (original == null) throw new AssertionError("Missing actual sticky Wi-Fi network status");
            NetworkInfo.DetailedState before = original.getDetailedState();
            original.setDetailedState(before == NetworkInfo.DetailedState.CONNECTED ? NetworkInfo.DetailedState.DISCONNECTED : NetworkInfo.DetailedState.CONNECTED,null,"mutated");
            NetworkInfo again = context.registerReceiver(null,filter).getParcelableExtra(WifiManager.EXTRA_NETWORK_INFO,NetworkInfo.class);
            if (again == original || again.getDetailedState() != before) throw new AssertionError("Wi-Fi sticky Parcelable was shared with its receiver");
            org.json.JSONObject saved = new org.json.JSONObject().put("pid",android.os.Process.myPid()).put("state",cached[0])
                .put("connected",cached[1] != 0).put("interfaceIndex",cached[2]).put("ipv4",cached[3]).put("netmask",cached[4]).put("samples",cached[8]);
            java.nio.file.Files.write(new java.io.File("/data/wifi-observation.json").toPath(),saved.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }
        WifiManager.MulticastLock first = wifi.createMulticastLock("Stone Wi-Fi probe"), second = wifi.createMulticastLock("Stone Wi-Fi probe");
        if (!permissions.contains("android.permission.CHANGE_WIFI_MULTICAST_STATE")) {
            try { first.acquire(); throw new AssertionError("Undeclared multicast permission was granted"); }
            catch (SecurityException expected) { }
            if (first.isHeld()) throw new AssertionError("Rejected multicast lock became held");
        } else try {
            first.acquire(); first.acquire(); second.setReferenceCounted(false); second.acquire(); second.acquire();
            if (!first.isHeld() || !second.isHeld()) throw new AssertionError("Multicast lock was not held");
            second.release();
            if (second.isHeld() || !first.isHeld()) throw new AssertionError("Same-tag multicast locks were confused");
            first.release(); if (!first.isHeld()) throw new AssertionError("Multicast reference count ended too early");
            first.release(); if (first.isHeld()) throw new AssertionError("Multicast reference count did not finish");
        } finally { if (second.isHeld()) second.release(); if (first.isHeld()) { first.setReferenceCounted(false); first.release(); } }
        for (String action : new String[]{WifiManager.WIFI_STATE_CHANGED_ACTION,WifiManager.NETWORK_STATE_CHANGED_ACTION}) {
            try { context.sendBroadcast(new Intent(action)); throw new AssertionError("Application forged a protected Wi-Fi broadcast"); }
            catch (SecurityException expected) { }
        }
        System.out.println("PASS image Wi-Fi: real cached state/address fields when declared, undeclared reads/locks denied, same-tag lock reference counts, isolated sticky data and protected broadcasts.");
    }
}
