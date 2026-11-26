package com.trebledot.stone.runtime;

import android.os.Binder;
import android.os.IBinder;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;

/** Host-owned Activity identity, distinct from ActivityGroup's local Binder tokens. */
final class ArtActivityToken {
    private ArtActivityToken() {}
    static IBinder create() {
        Binder local = new Binder();
        return (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class}, (proxy, method, args) -> {
            switch (method.getName()) {
                case "hashCode": return System.identityHashCode(proxy);
                case "equals": return proxy == args[0];
                case "toString": return "StoneActivityToken@" + Integer.toHexString(System.identityHashCode(proxy));
                default:
                    try { return method.invoke(local, args); }
                    catch (InvocationTargetException error) { throw error.getCause(); }
            }
        });
    }
}
