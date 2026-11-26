package com.trebledot.stone.runtime;

import android.content.Intent;
import android.net.DhcpInfo;
import android.net.NetworkInfo;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.IBinder;
import android.util.Log;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/** Shared WLAN information comes from the Service's asynchronous Windows cache. */
final class ArtWifi {
    private static native long[] queryHostState();
    private final ArtPackage apk;
    private final boolean available;
    private final Map<IBinder,String> multicast = new HashMap<>();
    private volatile long[] state;
    ArtWifi(ArtPackage apk, ArtBroadcasts broadcasts, boolean available) throws ReflectiveOperationException {
        this.apk = apk;
        this.available = available;
        state = read();
        publish(broadcasts,state,state);
        Executors.newSingleThreadScheduledExecutor(action -> {
            Thread thread = new Thread(action,"Stone Wi-Fi state"); thread.setDaemon(true); return thread;
        }).scheduleWithFixedDelay(() -> {
            try {
                long[] previous = state, next = read(); state = next;
                if (!Arrays.equals(Arrays.copyOf(previous,8),Arrays.copyOf(next,8))) publish(broadcasts,previous,next);
            } catch (Exception error) { Log.w("StoneWifi","Cannot publish cached WLAN state",error); }
        },1,1,TimeUnit.SECONDS);
    }
    static boolean available() {
        // Framework hardware features are immutable. During ART startup, before
        // creating the Application/UI, allow the asynchronous first sample to arrive.
        long deadline = android.os.SystemClock.uptimeMillis()+3000;
        do {
            long[] current = read();
            if (current[8] != 0) return current[2] != 0;
            try { Thread.sleep(10); }
            catch (InterruptedException cancelled) { Thread.currentThread().interrupt(); return false; }
        } while (android.os.SystemClock.uptimeMillis() < deadline);
        return false;
    }
    boolean hasFeature(String name, int version) {
        return available && android.content.pm.PackageManager.FEATURE_WIFI.equals(name) && version <= 0;
    }
    android.content.pm.FeatureInfo[] features() { return features(available); }
    static android.content.pm.FeatureInfo[] features(boolean available) {
        if (!available) return new android.content.pm.FeatureInfo[0];
        android.content.pm.FeatureInfo info = new android.content.pm.FeatureInfo();
        info.name = android.content.pm.PackageManager.FEATURE_WIFI;
        return new android.content.pm.FeatureInfo[]{info};
    }
    private static long[] read() {
        long[] value = queryHostState();
        if (value == null || value.length != 9 || value[0] < 0 || value[0] > 4 || value[1] < 0 || value[1] > 1 || value[8] < 0)
            throw new IllegalStateException("Invalid cached WLAN state");
        for (int index = 2; index < 8; ++index) if (value[index] < 0 || value[index] > 0xffffffffL)
            throw new IllegalStateException("Invalid cached WLAN address");
        return value;
    }
    private void permission(String name) {
        if (apk.packageInfo.requestedPermissions == null || !Arrays.asList(apk.packageInfo.requestedPermissions).contains(name))
            throw new SecurityException("The application did not declare "+name);
    }
    boolean granted(String name) {
        return ("android.permission.ACCESS_WIFI_STATE".equals(name) || "android.permission.CHANGE_WIFI_MULTICAST_STATE".equals(name))
            && apk.packageInfo.requestedPermissions != null && Arrays.asList(apk.packageInfo.requestedPermissions).contains(name);
    }
    private void owner(Object[] arguments) {
        if (arguments.length > 0 && arguments[0] instanceof String && !apk.applicationInfo.packageName.equals(arguments[0]))
            throw new SecurityException("Only the local application can query WLAN information");
    }
    private static WifiInfo info(long[] value) throws ReflectiveOperationException {
        // SSID/BSSID and station MAC are not collected by the host snapshot.
        WifiInfo result = new WifiInfo.Builder().build();
        WifiInfo.class.getMethod("setSupplicantState",SupplicantState.class).invoke(result,value[1] != 0 ? SupplicantState.COMPLETED : SupplicantState.DISCONNECTED);
        if (value[3] != 0) {
            int ip = (int)value[3];
            try { WifiInfo.class.getMethod("setInetAddress",InetAddress.class).invoke(result,InetAddress.getByAddress(
                new byte[]{(byte)ip,(byte)(ip>>8),(byte)(ip>>16),(byte)(ip>>24)})); }
            catch (java.net.UnknownHostException impossible) { throw new AssertionError(impossible); }
        }
        return result;
    }
    synchronized Object call(Object instance, Method method, Object[] arguments) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "isVerboseLoggingEnabled": return false;
            case "getVerboseLoggingLevel": return 0; // WIFI_VERBOSE_LOGGING_LEVEL_DISABLED.
            case "getWifiEnabledState": permission("android.permission.ACCESS_WIFI_STATE"); return (int)state[0];
            case "getSupportedFeatures": return state[2] != 0 ? 1L : 0L; // WIFI_FEATURE_INFRA only.
            case "getConnectionInfo":
                permission("android.permission.ACCESS_WIFI_STATE"); owner(arguments); return info(state);
            case "getDhcpInfo": {
                permission("android.permission.ACCESS_WIFI_STATE"); owner(arguments);
                long[] current = state;
                if (current[1] == 0 || current[3] == 0 || current[4] == 0) return null;
                DhcpInfo result = new DhcpInfo(); result.ipAddress = (int)current[3]; result.netmask = (int)current[4];
                result.gateway = (int)current[5]; result.dns1 = (int)current[6]; result.dns2 = (int)current[7];
                return result; // No invented DHCP server or lease duration.
            }
            case "acquireMulticastLock": {
                permission("android.permission.CHANGE_WIFI_MULTICAST_STATE");
                IBinder token = (IBinder)arguments[0]; String tag = (String)arguments[1];
                if (token == null || tag == null || tag.length() > 1024) throw new IllegalArgumentException("Invalid multicast token or tag");
                if (!multicast.containsKey(token) && multicast.size() >= 64) throw new IllegalStateException("Too many multicast locks");
                multicast.put(token,tag); return null;
            }
            case "releaseMulticastLock": {
                permission("android.permission.CHANGE_WIFI_MULTICAST_STATE");
                IBinder token = (IBinder)arguments[0]; String tag = (String)arguments[1];
                if (tag == null || !tag.equals(multicast.get(token))) throw new IllegalArgumentException("Unknown multicast lock");
                multicast.remove(token); return null;
            }
            default: throw new UnsupportedOperationException("ART Wi-Fi service: "+method.getName());
        }
    }
    private static void publish(ArtBroadcasts broadcasts, long[] previous, long[] next) throws ReflectiveOperationException {
        broadcasts.publishSystem(new Intent(WifiManager.WIFI_STATE_CHANGED_ACTION)
            .putExtra(WifiManager.EXTRA_WIFI_STATE,(int)next[0]).putExtra(WifiManager.EXTRA_PREVIOUS_WIFI_STATE,(int)previous[0]),true);
        NetworkInfo network = new NetworkInfo(android.net.ConnectivityManager.TYPE_WIFI,0,"WIFI","");
        NetworkInfo.class.getMethod("setIsAvailable",boolean.class).invoke(network,next[2] != 0);
        network.setDetailedState(next[1] != 0 ? NetworkInfo.DetailedState.CONNECTED : NetworkInfo.DetailedState.DISCONNECTED,null,null);
        broadcasts.publishSystem(new Intent(WifiManager.NETWORK_STATE_CHANGED_ACTION).putExtra(WifiManager.EXTRA_NETWORK_INFO,network),true);
    }
    static Intent copyBroadcast(Intent original) {
        Intent copy = new Intent(original);
        if (WifiManager.NETWORK_STATE_CHANGED_ACTION.equals(copy.getAction())) {
            NetworkInfo info = copy.getParcelableExtra(WifiManager.EXTRA_NETWORK_INFO,NetworkInfo.class);
            if (info != null) {
                android.os.Parcel parcel = android.os.Parcel.obtain();
                try { info.writeToParcel(parcel,0); parcel.setDataPosition(0); copy.putExtra(WifiManager.EXTRA_NETWORK_INFO,NetworkInfo.CREATOR.createFromParcel(parcel)); }
                finally { parcel.recycle(); }
            }
        }
        return copy;
    }
}
