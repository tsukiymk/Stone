package com.trebledot.stone.probe;

import android.content.Context;
import java.net.*;
import java.util.*;

/** Image interface APIs and real loopback multicast without an external peer. */
final class NetworkInterfaceProbe {
    private static void verifyMtu(NetworkInterface item) throws SocketException {
        try { if (item.getMTU() < 0) throw new AssertionError("Negative MTU"); }
        catch (SocketException error) {
            // Windows' unlimited loopback MTU is UINT32_MAX, outside Linux's signed ifreq field.
            Throwable cause = error.getCause();
            if (!(cause instanceof android.system.ErrnoException) || ((android.system.ErrnoException)cause).errno != 75) throw error;
        }
    }
    static void verify(Context context) throws Exception {
        InetAddress local = InetAddress.getByName("127.0.0.1"); NetworkInterface loop = null;
        long deadline = android.os.SystemClock.uptimeMillis()+20000;
        do {
            try { loop = NetworkInterface.getByInetAddress(local); }
            catch (SocketException pending) { if (android.os.SystemClock.uptimeMillis() >= deadline) throw pending; }
            if (loop != null) break;
            Thread.sleep(20);
        } while (android.os.SystemClock.uptimeMillis() < deadline);
        if (loop == null || !loop.isLoopback() || !loop.isUp() || loop.getIndex() <= 0
                || !NetworkInterface.getByIndex(loop.getIndex()).getName().equals(loop.getName())
                || NetworkInterface.getByName(loop.getName()).getIndex() != loop.getIndex())
            throw new AssertionError("Real loopback lookup/status/MTU failed");
        int addresses = 0, v6 = 0; Set<Integer> indexes = new HashSet<>();
        Enumeration<NetworkInterface> all = NetworkInterface.getNetworkInterfaces();
        while (all.hasMoreElements()) {
            NetworkInterface item = all.nextElement();
            if (item.getName().length() > 15 || item.getIndex() <= 0 || !indexes.add(item.getIndex())) throw new AssertionError("Invalid interface identity");
            item.isUp(); item.isLoopback(); item.isPointToPoint(); item.supportsMulticast();
            verifyMtu(item);
            for (InterfaceAddress address : item.getInterfaceAddresses()) {
                int max = address.getAddress() instanceof Inet4Address ? 32 : 128;
                if (address.getNetworkPrefixLength() < 0 || address.getNetworkPrefixLength() > max) throw new AssertionError("Invalid prefix");
                if (max == 128) { ++v6; if (address.getBroadcast() != null) throw new AssertionError("Invented IPv6 broadcast"); }
                ++addresses;
            }
        }
        if (!indexes.contains(loop.getIndex()) || addresses == 0 || NetworkInterface.getByName("stone-missing") != null)
            throw new AssertionError("Interface enumeration/absent lookup failed");
        android.net.wifi.WifiManager wifi = context.getSystemService(android.net.wifi.WifiManager.class);
        if (wifi != null && context.checkSelfPermission("android.permission.ACCESS_WIFI_STATE") == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            int ip = wifi.getConnectionInfo().getIpAddress();
            if (ip != 0 && NetworkInterface.getByInetAddress(InetAddress.getByAddress(new byte[]{(byte)ip,(byte)(ip>>8),(byte)(ip>>16),(byte)(ip>>24)})) == null)
                throw new AssertionError("Actual WLAN IPv4 missing from java.net interfaces");
        }
        InetAddress group = InetAddress.getByName("239.255.78.2");
        byte[] expected = ("stone-interface-"+android.os.Process.myPid()).getBytes(java.nio.charset.StandardCharsets.UTF_8);
        try (MulticastSocket receiver = new MulticastSocket((SocketAddress)null); MulticastSocket sender = new MulticastSocket((SocketAddress)null)) {
            receiver.setReuseAddress(true); receiver.bind(new InetSocketAddress(0)); receiver.setSoTimeout(3000);
            sender.setNetworkInterface(loop); sender.setTimeToLive(1);
            if (sender.getNetworkInterface().getIndex() != loop.getIndex()) throw new AssertionError("Multicast interface selection changed");
            InetSocketAddress endpoint = new InetSocketAddress(group,receiver.getLocalPort()); receiver.joinGroup(endpoint,loop);
            try {
                sender.send(new DatagramPacket(expected,expected.length,endpoint));
                byte[] bytes = new byte[128]; DatagramPacket packet = new DatagramPacket(bytes,bytes.length); receiver.receive(packet);
                if (!Arrays.equals(expected,Arrays.copyOf(bytes,packet.getLength())) || !local.equals(packet.getAddress()))
                    throw new AssertionError("Image multicast lost bytes or loopback source");
            } finally { receiver.leaveGroup(endpoint,loop); }
        }
        System.out.println("PASS image NetworkInterface: real enumeration, name/index/IP lookup, flags, MTU, prefixes (IPv6="+v6+") and actual loopback multicast bytes/source/join/leave.");
    }
}
