package com.trebledot.stone.runtime;

import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkRequest;
import android.os.Bundle;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.util.Log;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Exposes the Windows host's default connection to the in-process Android framework. */
final class ArtConnectivity {
    private static final int CONNECTED = 1;
    private static final int INTERNET = 2;
    private static final int UNMETERED = 4;
    private static final int TYPE_NONE = -1;
    private static final int HOST_NETWORK_ID = 100;

    private final Network network;
    private final Class<?> requestType = Class.forName("android.net.NetworkRequest$Type");
    private final Object[] requestTypes = requestType.getEnumConstants();
    private final Constructor<NetworkRequest> requestConstructor = NetworkRequest.class.getConstructor(
        NetworkCapabilities.class, int.class, int.class, requestType);
    private final Method addTransport = NetworkCapabilities.class.getMethod("addTransportType", int.class);
    private final Method setCapability = NetworkCapabilities.class.getMethod("setCapability", int.class, boolean.class);
    private final Method setAvailable = NetworkInfo.class.getMethod("setIsAvailable", boolean.class);
    private final int notVcnManaged = NetworkCapabilities.class.getField("NET_CAPABILITY_NOT_VCN_MANAGED").getInt(null);
    private final int callbackAvailable = callbackCode("CALLBACK_AVAILABLE");
    private final int callbackLost = callbackCode("CALLBACK_LOST");
    private final int callbackCapabilitiesChanged = callbackCode("CALLBACK_CAP_CHANGED");
    private final Map<NetworkRequest, Subscription> subscriptions = new HashMap<>();
    private final ScheduledExecutorService monitor = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "Stone connectivity");
        thread.setDaemon(true);
        return thread;
    });
    private volatile int hostNetworkState;
    private int nextRequestId = 1;

    ArtConnectivity() throws ReflectiveOperationException {
        network = Network.class.getConstructor(int.class).newInstance(HOST_NETWORK_ID);
        // Seed the snapshot before the application starts. Windows network queries
        // during the Activity's lifetime belong to the monitor, outside the subscription lock.
        hostNetworkState = queryHostNetworkState();
        monitor.scheduleWithFixedDelay(() -> {
            hostNetworkState = queryHostNetworkState();
            publish();
        }, 1, 1, TimeUnit.SECONDS);
    }

    private static native int queryHostNetworkState();

    Object call(Object instance, Method method, Object[] arguments) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "getActiveNetwork": return hostNetworkState != 0 ? network : null;
            case "getActiveNetworkInfo": return networkInfo(hostNetworkState);
            case "getNetworkInfoForUid":
                if((Integer)arguments[1]!=android.os.Process.myUid() || (Boolean)arguments[2])
                    throw new SecurityException("Network information is restricted to the current UID");
                return network.equals(arguments[0])?networkInfo(hostNetworkState):null;
            case "getNetworkCapabilities":
                return network.equals(arguments[0]) ? capabilities(hostNetworkState) : null;
            case "getLinkProperties":
                return network.equals(arguments[0]) && hostNetworkState != 0 ? new LinkProperties() : null;
            case "getAllNetworks": return hostNetworkState != 0 ? new Network[]{network} : new Network[0];
            case "isActiveNetworkMetered": return (hostNetworkState & UNMETERED) == 0;
            case "getEnabledConnectivityManagerFeatures": return 0L;
            case "listenForNetwork":
                return register(findRequestType("LISTEN"), (NetworkCapabilities) arguments[0], (Messenger) arguments[1]);
            case "requestNetwork": {
                Object type = requestTypes[(Integer) arguments[2]];
                switch (((Enum<?>) type).name()) {
                    case "TRACK_DEFAULT":
                    case "TRACK_SYSTEM_DEFAULT":
                        return register(type, null, (Messenger) arguments[3]);
                    case "LISTEN_FOR_BEST":
                        return register(type, (NetworkCapabilities) arguments[1], (Messenger) arguments[3]);
                    default: return null;
                }
            }
            case "releaseNetworkRequest":
                unregister((NetworkRequest) arguments[0]);
                return null;
            default: throw new UnsupportedOperationException("ART connectivity service: " + method.getName());
        }
    }

    private NetworkInfo networkInfo(int state) throws ReflectiveOperationException {
        if ((state & CONNECTED) == 0) return null;
        NetworkInfo info = new NetworkInfo(ConnectivityManager.TYPE_ETHERNET, 0, "ETHERNET", "");
        setAvailable.invoke(info, true);
        info.setDetailedState(NetworkInfo.DetailedState.CONNECTED, null, null);
        return info;
    }

    private NetworkCapabilities capabilities(int state) throws ReflectiveOperationException {
        if ((state & CONNECTED) == 0) return null;
        // Android sees one virtual wired connection. Windows selects the physical route.
        NetworkCapabilities capabilities = new NetworkCapabilities();
        addTransport.invoke(capabilities, NetworkCapabilities.TRANSPORT_ETHERNET);
        setCapability.invoke(capabilities, NetworkCapabilities.NET_CAPABILITY_INTERNET, (state & INTERNET) != 0);
        setCapability.invoke(capabilities, NetworkCapabilities.NET_CAPABILITY_VALIDATED, (state & INTERNET) != 0);
        setCapability.invoke(capabilities, NetworkCapabilities.NET_CAPABILITY_NOT_METERED, (state & UNMETERED) != 0);
        setCapability.invoke(capabilities, NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED, true);
        // This host-owned wired network never belongs to Android's VCN service.
        // Modern NetworkRequest.Builder adds this requirement implicitly.
        setCapability.invoke(capabilities, notVcnManaged, true);
        return capabilities;
    }

    private Object findRequestType(String name) {
        for (Object type : requestTypes) if (((Enum<?>) type).name().equals(name)) return type;
        throw new IllegalArgumentException("Unknown network request type " + name);
    }

    private static int callbackCode(String field) throws ReflectiveOperationException {
        return ConnectivityManager.class.getField(field).getInt(null);
    }

    private synchronized NetworkRequest register(Object type, NetworkCapabilities need, Messenger messenger)
            throws ReflectiveOperationException {
        NetworkCapabilities filter = need == null ? new NetworkCapabilities() : new NetworkCapabilities(need);
        NetworkRequest request = requestConstructor.newInstance(filter, TYPE_NONE, nextRequestId++, type);
        subscriptions.put(request, new Subscription(messenger));
        // Messenger enqueues delivery. ConnectivityManager installs its callback under
        // the same framework lock that the receiver takes before invoking the app.
        monitor.execute(this::publish);
        return request;
    }

    private synchronized void unregister(NetworkRequest request) {
        subscriptions.remove(request);
    }

    private synchronized void publish() {
        if (subscriptions.isEmpty()) return;
        try {
            int state = hostNetworkState;
            NetworkCapabilities current = capabilities(state);
            subscriptions.entrySet().removeIf(entry -> {
                NetworkRequest request = entry.getKey();
                Subscription subscription = entry.getValue();
                int matched = current != null && request.canBeSatisfiedBy(current) ? state : 0;
                if (matched == subscription.state) return false;
                int code = matched == 0 ? callbackLost
                    : subscription.state == 0 ? callbackAvailable : callbackCapabilitiesChanged;
                try {
                    deliver(subscription.messenger, request, code, current);
                    subscription.state = matched;
                    return false;
                } catch (RemoteException disconnected) { return true; }
            });
        } catch (ReflectiveOperationException error) {
            Log.e("StoneConnectivity", "Could not publish Windows network capabilities", error);
        }
    }

    private void deliver(Messenger messenger, NetworkRequest request, int code, NetworkCapabilities capabilities)
            throws RemoteException {
        Bundle data = new Bundle();
        data.putParcelable("NetworkRequest", request);
        data.putParcelable("Network", network);
        if (code != callbackLost) data.putParcelable("NetworkCapabilities", new NetworkCapabilities(capabilities));
        if (code == callbackAvailable) data.putParcelable("LinkProperties", new LinkProperties());
        Message message = Message.obtain();
        message.what = code;
        message.setData(data);
        messenger.send(message);
    }

    private static final class Subscription {
        final Messenger messenger;
        int state;

        Subscription(Messenger messenger) { this.messenger = messenger; }
    }
}
