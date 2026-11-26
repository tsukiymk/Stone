package com.trebledot.stone.probe;

/** Public image socket APIs exercising TCP bytes and Android's deferred-close marker. */
final class TcpServerProbe {
    static void verify() throws Exception {
        java.net.InetAddress loop = java.net.InetAddress.getByName("127.0.0.1");
        java.util.concurrent.atomic.AtomicReference<Throwable> clientError = new java.util.concurrent.atomic.AtomicReference<>();
        try (java.net.ServerSocket server = new java.net.ServerSocket(0,1,loop)) {
            server.setSoTimeout(3000);
            Thread client = new Thread(() -> {
                try (java.net.Socket socket = new java.net.Socket(loop,server.getLocalPort())) {
                    socket.setSoTimeout(3000); socket.getOutputStream().write(42);
                    if (socket.getInputStream().read() != 43) throw new AssertionError("Image TCP response byte changed");
                } catch (Throwable error) { clientError.set(error); }
            },"Stone image TCP client");
            client.setDaemon(true); client.start();
            try (java.net.Socket socket = server.accept()) {
                socket.setSoTimeout(3000);
                if (socket.getInputStream().read() != 42) throw new AssertionError("Image accepted TCP request byte changed");
                socket.getOutputStream().write(43);
            }
            client.join(4000);
            if (client.isAlive() || clientError.get() != null) throw new AssertionError("Image TCP client failed",clientError.get());
        }
        for (int round = 0; round < 2; ++round) {
            java.net.ServerSocket server = new java.net.ServerSocket(0,1,loop);
            int port = server.getLocalPort();
            java.util.concurrent.atomic.AtomicReference<Throwable> result = new java.util.concurrent.atomic.AtomicReference<>();
            Thread listener = new Thread(() -> {
                try (java.net.Socket unexpected = server.accept()) { result.set(new AssertionError("Closed listener accepted an unexpected connection")); }
                catch (java.net.SocketException expected) { }
                catch (Throwable error) { result.set(error); }
            },"Stone deferred socket close");
            listener.setDaemon(true); listener.start();
            try {
                long deadline = android.os.SystemClock.uptimeMillis()+3000;
                boolean accepting = false;
                do {
                    for (StackTraceElement item : listener.getStackTrace()) if (item.getMethodName().equals("socketAccept")) accepting = true;
                    if (accepting) break;
                    Thread.sleep(10);
                } while (android.os.SystemClock.uptimeMillis() < deadline);
                if (!accepting) throw new AssertionError("Image listener did not enter actual socketAccept");
                server.close(); listener.join(2000);
                if (listener.isAlive()) throw new AssertionError("Image ServerSocket.close left accept blocked: "+java.util.Arrays.toString(listener.getStackTrace()));
                if (result.get() != null) throw new AssertionError("Image socket close returned another failure",result.get());
                try (java.net.ServerSocket rebound = new java.net.ServerSocket(port,1,loop)) {
                    if (rebound.getLocalPort() != port) throw new AssertionError("Closed listening port could not be reused");
                }
            } finally { server.close(); }
        }
        System.out.println("PASS image TCP: actual request/response bytes, blocked accept exits on close, listener joins and the same port rebinds twice.");
    }
}
