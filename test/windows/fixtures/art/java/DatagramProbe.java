package com.trebledot.stone.probe;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.net.SocketAddress;
import java.net.SocketTimeoutException;
import java.util.Arrays;

/** Image java.net APIs over real Windows sockets; all payload traffic stays on loopback. */
final class DatagramProbe {
    static void verify() throws Exception {
        InetAddress loop = InetAddress.getByName("127.0.0.1");
        byte[] expected = new byte[]{0,1,(byte)0x80,(byte)0xff,2,0,3};
        try (DatagramSocket receiver = new DatagramSocket(new InetSocketAddress(loop,0));
             DatagramSocket sender = new DatagramSocket(new InetSocketAddress(loop,0))) {
            receiver.setSoTimeout(2000);
            sender.send(new DatagramPacket(expected,expected.length,loop,receiver.getLocalPort()));
            byte[] bytes = new byte[32]; DatagramPacket packet = new DatagramPacket(bytes,bytes.length);
            receiver.receive(packet);
            if (packet.getLength() != expected.length || packet.getPort() != sender.getLocalPort()
                    || !loop.equals(packet.getAddress()) || !Arrays.equals(expected,Arrays.copyOf(bytes,packet.getLength())))
                throw new AssertionError("Image DatagramSocket lost its payload or source address");
            receiver.setSoTimeout(200);
            long before = android.os.SystemClock.uptimeMillis();
            try { receiver.receive(packet); throw new AssertionError("Empty UDP socket unexpectedly received a packet"); }
            catch (SocketTimeoutException correct) {
                if (android.os.SystemClock.uptimeMillis()-before < 100) throw new AssertionError("UDP timeout returned before its deadline");
            }
        }
        try (MulticastSocket multicast = new MulticastSocket((SocketAddress)null)) {
            multicast.setTimeToLive(23);
            multicast.setLoopbackMode(false);
            if (multicast.getTimeToLive() != 23 || multicast.getLoopbackMode())
                throw new AssertionError("Image MulticastSocket did not retain its TTL or enabled loopback");
        }
        System.out.println("PASS image java.net datagrams: real loopback bytes/source/port, bounded receive timeout, multicast creation, TTL and enabled loopback; group delivery is checked separately.");
    }
}
